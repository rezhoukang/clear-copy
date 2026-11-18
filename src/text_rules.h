/* In-place UTF-16 transformations. Every rule only shortens the input. */
#define RULE_COUNT 18
#define RULE_BIT(index) (1u << (index))
enum {
    RULE_TRIM_START, RULE_TRIM_END, RULE_COLLAPSE_SPACE, RULE_DELETE_SPACE,
    RULE_FULLWIDTH, RULE_NBSP, RULE_TAB_SPACE, RULE_TAB_DELETE,
    RULE_LEADING_BLANK, RULE_TRAILING_BLANK, RULE_COLLAPSE_BLANK, RULE_DELETE_BLANK,
    RULE_NEWLINE_SPACE, RULE_NEWLINE_DELETE, RULE_ZERO_WIDTH, RULE_BOM,
    RULE_IMAGE, RULE_FILE
};
#define MEDIA_RULES (RULE_BIT(RULE_IMAGE) | RULE_BIT(RULE_FILE))
#define TEXT_RULES ((1u << RULE_IMAGE) - 1u)
#define ALL_RULES ((1u << RULE_COUNT) - 1u)

static BOOL line_break(WCHAR ch) { return ch == L'\r' || ch == L'\n'; }

static SIZE_T after_break(const WCHAR *text, SIZE_T pos, SIZE_T length)
{
    if (pos < length && text[pos] == L'\r' && pos + 1 < length && text[pos + 1] == L'\n') return pos + 2;
    return pos < length ? pos + 1 : pos;
}

static SIZE_T transform_text(WCHAR *text, SIZE_T length, DWORD rules)
{
    SIZE_T read, write = 0, start, end, next, first = 0, last = length;
    BOOL previous_blank = FALSE, kept_line = FALSE;
    SIZE_T previous_break_start = 0;
    /* Convert only explicitly selected code points; preserve ZWJ/emoji. */
    for (read = 0; read < length; ++read) {
        WCHAR ch = text[read];
        if ((ch == 0x200b && (rules & RULE_BIT(RULE_ZERO_WIDTH))) ||
            (ch == 0xfeff && (rules & RULE_BIT(RULE_BOM))) ||
            (ch == L'\t' && (rules & RULE_BIT(RULE_TAB_DELETE)))) continue;
        if ((ch == 0x3000 && (rules & RULE_BIT(RULE_FULLWIDTH))) ||
            (ch == 0x00a0 && (rules & RULE_BIT(RULE_NBSP))) ||
            (ch == L'\t' && (rules & RULE_BIT(RULE_TAB_SPACE)))) ch = L' ';
        text[write++] = ch;
    }
    length = write;
    /* Trim each original line before joining any lines. */
    write = 0;
    for (read = 0; read < length; read = next) {
        start = read;
        for (end = read; end < length && !line_break(text[end]); ++end) {}
        next = after_break(text, end, length);
        if (rules & RULE_BIT(RULE_TRIM_START)) while (start < end && text[start] == L' ') ++start;
        read = end;
        if (rules & RULE_BIT(RULE_TRIM_END)) while (read > start && text[read - 1] == L' ') --read;
        while (start < read) text[write++] = text[start++];
        while (end < next) text[write++] = text[end++];
    }
    length = write;
    /* Find the first and last lines containing something other than space/Tab. */
    if (rules & (RULE_BIT(RULE_LEADING_BLANK) | RULE_BIT(RULE_TRAILING_BLANK))) {
        BOOL found = FALSE;
        first = length;
        last = 0;
        for (read = 0; read < length; read = next) {
            BOOL blank = TRUE;
            for (end = read; end < length && !line_break(text[end]); ++end)
                if (text[end] != L' ' && text[end] != L'\t') blank = FALSE;
            next = after_break(text, end, length);
            if (!blank) { if (!found) first = read; last = end; found = TRUE; }
        }
        if (!(rules & RULE_BIT(RULE_LEADING_BLANK))) first = 0;
        if (!(rules & RULE_BIT(RULE_TRAILING_BLANK))) last = length;
        if (first > last) first = last;
    } else { first = 0; last = length; }
    write = 0;
    for (read = first; read < last; read = next) {
        BOOL blank = TRUE, skip;
        for (end = read; end < last && !line_break(text[end]); ++end)
            if (text[end] != L' ' && text[end] != L'\t') blank = FALSE;
        next = after_break(text, end, last);
        skip = blank && ((rules & RULE_BIT(RULE_DELETE_BLANK)) ||
                         (previous_blank && (rules & RULE_BIT(RULE_COLLAPSE_BLANK))));
        if (!skip) {
            previous_break_start = write + end - read;
            for (start = read; start < next; ++start) text[write++] = text[start];
            previous_blank = blank;
            kept_line = TRUE;
        } else if (next == last && end == last && kept_line) {
            /* A trailing whitespace-only line has no own separator. */
            write = previous_break_start;
        }
    }
    length = write;
    write = 0;
    for (read = 0; read < length; ++read) {
        WCHAR ch = text[read];
        if (line_break(ch) && (rules & (RULE_BIT(RULE_NEWLINE_SPACE) | RULE_BIT(RULE_NEWLINE_DELETE)))) {
            read = after_break(text, read, length) - 1;
            if (rules & RULE_BIT(RULE_NEWLINE_DELETE)) continue;
            ch = L' ';
        }
        if (ch == L' ') {
            if (rules & RULE_BIT(RULE_DELETE_SPACE)) continue;
            if ((rules & RULE_BIT(RULE_COLLAPSE_SPACE)) && write && text[write - 1] == L' ') continue;
        }
        text[write++] = ch;
    }
    text[write] = 0;
    return write;
}
