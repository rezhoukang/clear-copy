/* Clipboard formats retained during a selective rewrite. Unknown mixed formats
   abort before EmptyClipboard; no delayed-rendering/OLE objects are guessed. */
typedef struct { UINT format; HANDLE handle; } MEDIA_COPY;
typedef struct { MEDIA_COPY items[16]; unsigned count; SIZE_T bytes; } MEDIA_SNAPSHOT;

static BOOL image_format(UINT format)
{
    WCHAR name[80];
    static const WCHAR *names[] = { L"PNG", L"image/png", L"JPEG", L"JFIF", L"image/jpeg",
        L"GIF", L"image/gif", L"TIFF", L"image/tiff", L"WebP", L"image/webp", L"image/bmp" };
    unsigned i;
    if (format == CF_BITMAP || format == CF_DIB || format == CF_DIBV5 ||
        format == CF_PALETTE || format == CF_ENHMETAFILE || format == CF_METAFILEPICT) return TRUE;
    if (format < 0xc000 || !GetClipboardFormatNameW(format, name, ARRAYSIZE(name))) return FALSE;
    for (i = 0; i < ARRAYSIZE(names); ++i) if (!lstrcmpiW(name, names[i])) return TRUE;
    return FALSE;
}

static BOOL file_format(UINT format)
{
    WCHAR name[80];
    if (format == CF_HDROP || (shell_idlist_format && format == shell_idlist_format)) return TRUE;
    if (format < 0xc000 || !GetClipboardFormatNameW(format, name, ARRAYSIZE(name))) return FALSE;
    return !lstrcmpW(name, L"FileName") || !lstrcmpW(name, L"FileNameW") ||
           !lstrcmpW(name, L"Preferred DropEffect") || !lstrcmpW(name, L"Performed DropEffect") ||
           !lstrcmpW(name, L"Paste Succeeded");
}

static BOOL has_media(void)
{
    UINT format = 0;
    while ((format = EnumClipboardFormats(format)) != 0)
        if (image_format(format) || file_format(format)) return TRUE;
    return FALSE;
}

static void free_media(MEDIA_SNAPSHOT *snapshot)
{
    unsigned i;
    for (i = 0; i < snapshot->count; ++i) {
        MEDIA_COPY *item = &snapshot->items[i];
        if (!item->handle) continue;
        if (item->format == CF_BITMAP || item->format == CF_PALETTE) DeleteObject(item->handle);
        else if (item->format == CF_ENHMETAFILE) DeleteEnhMetaFile(item->handle);
        else GlobalFree(item->handle);
    }
}

static HGLOBAL copy_global(HGLOBAL source, SIZE_T *total)
{
    SIZE_T size = GlobalSize(source);
    HGLOBAL copy;
    void *from, *to;
    if (!size || size > MAX_TEXT_BYTES || *total > MAX_TEXT_BYTES - size) return NULL;
    from = GlobalLock(source);
    if (!from) return NULL;
    copy = GlobalAlloc(GMEM_MOVEABLE, size);
    to = copy ? GlobalLock(copy) : NULL;
    if (to) { CopyMemory(to, from, size); GlobalUnlock(copy); *total += size; }
    else if (copy) { GlobalFree(copy); copy = NULL; }
    GlobalUnlock(source);
    return copy;
}

static BOOL snapshot_media(MEDIA_SNAPSHOT *snapshot, DWORD rules, BOOL *removed)
{
    UINT format = 0;
    BOOL mixed = has_media();
    ZeroMemory(snapshot, sizeof(*snapshot));
    *removed = FALSE;
    while ((format = EnumClipboardFormats(format)) != 0) {
        BOOL picture = image_format(format), file = file_format(format);
        HANDLE source, copy = NULL;
        WCHAR name[80];
        if (!picture && !file) {
            if (format == CF_UNICODETEXT || format == CF_TEXT || format == CF_OEMTEXT || format == CF_LOCALE) continue;
            if (format == html_format || format == rtf_format) { *removed = TRUE; continue; }
            /* Virtual files and embedded OLE objects cannot be flattened safely. */
            if (mixed) goto fail;
            name[0] = 0;
            GetClipboardFormatNameW(format, name, ARRAYSIZE(name));
            if (!lstrcmpW(name, L"FileGroupDescriptor") || !lstrcmpW(name, L"FileGroupDescriptorW") ||
                !lstrcmpW(name, L"FileContents") || !lstrcmpW(name, L"Embedded Object") ||
                !lstrcmpW(name, L"Embed Source") || !lstrcmpW(name, L"Object Descriptor")) goto fail;
            *removed = TRUE;
            continue;
        }
        if ((picture && (rules & RULE_BIT(RULE_IMAGE))) || (file && (rules & RULE_BIT(RULE_FILE)))) {
            *removed = TRUE;
            continue;
        }
        if (snapshot->count == ARRAYSIZE(snapshot->items)) goto fail;
        /* DIB formats can recreate CF_BITMAP; avoid duplicating device handles. */
        if (format == CF_BITMAP && (IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_DIBV5))) continue;
        source = GetClipboardData(format);
        if (!source) goto fail;
        if (format == CF_BITMAP) copy = CopyImage(source, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
        else if (format == CF_ENHMETAFILE) copy = CopyEnhMetaFileW(source, NULL);
        else if (format == CF_PALETTE || format == CF_METAFILEPICT) goto fail;
        else copy = copy_global(source, &snapshot->bytes);
        if (!copy) goto fail;
        snapshot->items[snapshot->count].format = format;
        snapshot->items[snapshot->count++].handle = copy;
    }
    return TRUE;
fail:
    free_media(snapshot);
    snapshot->count = 0;
    return FALSE;
}

static void restore_media(MEDIA_SNAPSHOT *snapshot)
{
    unsigned i;
    for (i = 0; i < snapshot->count; ++i) {
        MEDIA_COPY *item = &snapshot->items[i];
        if (SetClipboardData(item->format, item->handle) || SetClipboardData(item->format, item->handle)) item->handle = NULL;
    }
    free_media(snapshot);
}
