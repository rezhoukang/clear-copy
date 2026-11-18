#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define _WIN32_IE 0x0600
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <strsafe.h>
#include <wchar.h>
#include "resource.h"
#include "text_rules.h"

#define APP_CLASS L"ClearCopy.MessageWindow.v1"
#define APP_MUTEX L"Local\\ClearCopy.SingleInstance.v1"
#define CONFIG_KEY L"Software\\ClearCopy"
#ifndef RUN_KEY
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#endif
#define WM_TRAY (WM_APP + 1)
#define WM_SETTINGS (WM_APP + 2)
#define TIMER_CLIPBOARD 1
#define TIMER_TRAY 2
#define RETRY_LIMIT 20
#define MAX_TEXT_BYTES (64u * 1024u * 1024u)

static HINSTANCE instance;
static HWND main_window;
static HWND settings_window;
static HWND special_window;
static HFONT wrench_font;
static HICON active_icon, active_small_icon, paused_icon;
static HWND status_tooltip;
static HFONT status_font;
static HFONT menu_status_font;
static HMENU active_menu;
static BOOL enabled = TRUE;
static BOOL special_enabled = FALSE;
static DWORD special_rules;
static HWND rule_tooltip;
static BOOL listening;
static BOOL tray_added;
static UINT taskbar_created;
static DWORD own_sequence;
static DWORD pending_sequence;
static unsigned retries;
static UINT html_format, rtf_format, shell_idlist_format;

static void report_error(HWND owner, const WCHAR *message, DWORD code)
{
    WCHAR system_message[512] = L"", buffer[1024];
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, code, 0, system_message, 512, NULL);
    StringCchPrintfW(buffer, 1024, L"%ls\n\n错误 %lu：%ls", message, (unsigned long)code, system_message);
    MessageBoxW(owner, buffer, L"Clear Copy", MB_OK | MB_ICONERROR);
}

static void load_config(void)
{
    HKEY key;
    DWORD value, size = sizeof(value), type;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, CONFIG_KEY, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, L"Enabled", NULL, &type, (BYTE *)&value, &size) == ERROR_SUCCESS && type == REG_DWORD)
            enabled = value != 0;
        size = sizeof(value);
        if (RegQueryValueExW(key, L"CleanMixed", NULL, &type, (BYTE *)&value, &size) == ERROR_SUCCESS && type == REG_DWORD && value) {
            special_enabled = TRUE;
            special_rules = MEDIA_RULES;
        }
        size = sizeof(value);
        if (RegQueryValueExW(key, L"SpecialEnabled", NULL, &type, (BYTE *)&value, &size) == ERROR_SUCCESS && type == REG_DWORD)
            special_enabled = value != 0;
        size = sizeof(value);
        if (RegQueryValueExW(key, L"SpecialRules", NULL, &type, (BYTE *)&value, &size) == ERROR_SUCCESS && type == REG_DWORD)
            special_rules = value & ALL_RULES;
        RegCloseKey(key);
    }
}

static LSTATUS save_option(const WCHAR *name, DWORD value)
{
    HKEY key;
    DWORD data = value;
    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, CONFIG_KEY, 0, NULL, 0,
                                    KEY_SET_VALUE, NULL, &key, NULL);
    if (result != ERROR_SUCCESS) return result;
    result = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&data, sizeof(data));
    RegCloseKey(key);
    return result;
}

static LSTATUS save_enabled(BOOL value)
{
    return save_option(L"Enabled", value);
}

static BOOL startup_enabled(void)
{
    HKEY key;
    LSTATUS result;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return FALSE;
    result = RegQueryValueExW(key, L"ClearCopy", NULL, NULL, NULL, NULL);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

static LSTATUS set_startup(BOOL value)
{
    HKEY key;
    LSTATUS result;
    WCHAR path[32768], command[32772];
    DWORD length;
    if (value) {
        length = GetModuleFileNameW(NULL, path, 32768);
        if (!length || length >= 32768) return ERROR_INSUFFICIENT_BUFFER;
        if (FAILED(StringCchPrintfW(command, 32772, L"\"%ls\"", path))) return ERROR_INSUFFICIENT_BUFFER;
        result = RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
        if (result != ERROR_SUCCESS) return result;
        result = RegSetValueExW(key, L"ClearCopy", 0, REG_SZ, (const BYTE *)command,
                               (DWORD)((wcslen(command) + 1) * sizeof(WCHAR)));
    } else {
        result = RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key);
        if (result == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
        if (result != ERROR_SUCCESS) return result;
        result = RegDeleteValueW(key, L"ClearCopy");
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result;
}

static void layout_settings_status(void)
{
    HWND label = GetDlgItem(settings_window, IDC_STATUS_TEXT);
    HWND indicator = GetDlgItem(settings_window, IDC_STATUS);
    RECT client, metrics = {0, 0, 16, 10};
    WCHAR text[16];
    SIZE extent;
    HDC dc = GetDC(label);
    HFONT font = (HFONT)SendMessageW(label, WM_GETFONT, 0, 0);
    HGDIOBJ previous = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    int height, dot, gap, x, y;
    GetWindowTextW(label, text, ARRAYSIZE(text));
    GetTextExtentPoint32W(dc, text, lstrlenW(text), &extent);
    SelectObject(dc, previous);
    ReleaseDC(label, dc);
    GetClientRect(settings_window, &client);
    MapDialogRect(settings_window, &metrics);
    height = extent.cy + 2;
    dot = MulDiv(height, 2, 5);
    gap = dot;
    x = client.right - metrics.right - extent.cx;
    y = client.bottom - metrics.bottom - height;
    /* Tight text width keeps both status variants anchored to the same right
       edge. The dot and label share a vertically centered row. */
    SetWindowPos(label, NULL, x, y, extent.cx, height, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(indicator, NULL, x - gap - dot, y, dot, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void refresh_settings_status(void)
{
    if (settings_window) {
        HWND indicator = GetDlgItem(settings_window, IDC_STATUS);
        const WCHAR *description = enabled ? L"正在监听剪贴板" : L"已暂停监听剪贴板";
        /* Owner-drawn static keeps its accessible name without visible text. */
        SetWindowTextW(indicator, description);
        SetDlgItemTextW(settings_window, IDC_STATUS_TEXT, enabled ? L"监听中" : L"空闲");
        layout_settings_status();
        InvalidateRect(indicator, NULL, TRUE);
        if (status_tooltip) {
            TOOLINFOW tool;
            ZeroMemory(&tool, sizeof(tool));
            tool.cbSize = sizeof(tool);
            tool.hwnd = settings_window;
            tool.uId = (UINT_PTR)indicator;
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.lpszText = (LPWSTR)description;
            SendMessageW(status_tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tool);
        }
    }
}

static void update_tray(BOOL add)
{
    NOTIFYICONDATAW data;
    ZeroMemory(&data, sizeof(data));
    data.cbSize = sizeof(data);
    data.hWnd = main_window;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = WM_TRAY;
    data.hIcon = enabled ? active_small_icon : paused_icon;
    StringCchCopyW(data.szTip, ARRAYSIZE(data.szTip),
                   enabled ? L"Clear Copy · 自动清洗中\n右键打开菜单，双击打开设置" : L"Clear Copy · 已暂停\n右键打开菜单，双击打开设置");
    if (Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &data)) {
        tray_added = TRUE;
        KillTimer(main_window, TIMER_TRAY);
        if (add) {
            data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &data);
        }
    } else {
        tray_added = FALSE;
        SetTimer(main_window, TIMER_TRAY, 2000, NULL);
    }
}

static void apply_enabled(BOOL value)
{
    enabled = value;
    KillTimer(main_window, TIMER_CLIPBOARD);
    retries = 0;
    update_tray(!tray_added);
    refresh_settings_status();
}

static void apply_special_enabled(BOOL value)
{
    special_enabled = value;
    KillTimer(main_window, TIMER_CLIPBOARD);
    retries = 0;
    if (settings_window)
        CheckDlgButton(settings_window, IDC_SPECIAL_ENABLED, value ? BST_CHECKED : BST_UNCHECKED);
}

#include "media.h"

static BOOL should_clean(void)
{
    UINT format = 0;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return FALSE;
    if (!special_enabled && has_media()) return FALSE;
    if (special_enabled && (special_rules & TEXT_RULES)) return TRUE;
    while ((format = EnumClipboardFormats(format)) != 0) {
        if (format != CF_UNICODETEXT && format != CF_TEXT && format != CF_OEMTEXT && format != CF_LOCALE)
            return TRUE;
    }
    return FALSE;
}

/* A retry holds no cached text. Recheck the sequence after acquiring the lock,
   so a delayed retry cannot write an older copy over a newer one. */
static void clean_clipboard(void)
{
    HANDLE source;
    HGLOBAL copy;
    const WCHAR *text;
    WCHAR *destination;
    SIZE_T bytes, units, length;
    DWORD sequence;
    MEDIA_SNAPSHOT media;
    BOOL removed, changed;
    if (!enabled) return;
    KillTimer(main_window, TIMER_CLIPBOARD);
    sequence = GetClipboardSequenceNumber();
    if (sequence == own_sequence || sequence != pending_sequence) return;
    if (!OpenClipboard(main_window)) {
        if (++retries <= RETRY_LIMIT) SetTimer(main_window, TIMER_CLIPBOARD, 15, NULL);
        return;
    }
    if (GetClipboardSequenceNumber() != pending_sequence || !should_clean()) {
        CloseClipboard();
        return;
    }
    source = GetClipboardData(CF_UNICODETEXT);
    if (!source) { CloseClipboard(); return; }
    bytes = GlobalSize(source);
    if (bytes < sizeof(WCHAR) || bytes > MAX_TEXT_BYTES || bytes % sizeof(WCHAR)) { CloseClipboard(); return; }
    text = (const WCHAR *)GlobalLock(source);
    if (!text) { CloseClipboard(); return; }
    units = bytes / sizeof(WCHAR);
    for (length = 0; length < units && text[length]; ++length) {}
    if (length == units || (length == 0 && has_media())) {
        GlobalUnlock(source);
        CloseClipboard();
        return;
    }
    bytes = (length + 1) * sizeof(WCHAR);
    copy = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!copy) { GlobalUnlock(source); CloseClipboard(); return; }
    destination = (WCHAR *)GlobalLock(copy);
    if (!destination) { GlobalFree(copy); GlobalUnlock(source); CloseClipboard(); return; }
    CopyMemory(destination, text, bytes);
    if (special_enabled) transform_text(destination, length, special_rules);
    /* Locale-aware lstrcmpW can consider BOM and other characters ignorable. */
    changed = wcscmp(destination, text) != 0;
    GlobalUnlock(copy);
    GlobalUnlock(source);
    if (!snapshot_media(&media, special_enabled ? special_rules : 0, &removed)) {
        GlobalFree(copy);
        CloseClipboard();
        return;
    }
    if (!changed && !removed) {
        free_media(&media);
        GlobalFree(copy);
        CloseClipboard();
        return;
    }
    if (EmptyClipboard()) {
        if (!SetClipboardData(CF_UNICODETEXT, copy) && !SetClipboardData(CF_UNICODETEXT, copy)) GlobalFree(copy);
        restore_media(&media);
        own_sequence = GetClipboardSequenceNumber();
    } else {
        free_media(&media);
        GlobalFree(copy);
    }
    CloseClipboard();
}

static void draw_status_dot(HDC dc, int left, int top, int diameter, COLORREF background)
{
    COLORREF foreground = enabled ? RGB(22, 163, 74) : RGB(132, 138, 145);
    int x, y;
    /* 4x4 coverage sampling keeps a tiny native GDI dot round rather
       than diamond-shaped, without an image or graphics dependency. */
    for (y = 0; y < diameter; ++y) {
        for (x = 0; x < diameter; ++x) {
            int sx, sy, coverage = 0;
            for (sy = 0; sy < 4; ++sy) {
                for (sx = 0; sx < 4; ++sx) {
                    int dx = x * 8 + sx * 2 + 1 - diameter * 4;
                    int dy = y * 8 + sy * 2 + 1 - diameter * 4;
                    if (dx * dx + dy * dy <= diameter * diameter * 16) ++coverage;
                }
            }
            if (coverage) {
                int red = (GetRValue(foreground) * coverage + GetRValue(background) * (16 - coverage) + 8) / 16;
                int green = (GetGValue(foreground) * coverage + GetGValue(background) * (16 - coverage) + 8) / 16;
                int blue = (GetBValue(foreground) * coverage + GetBValue(background) * (16 - coverage) + 8) / 16;
                SetPixelV(dc, left + x, top + y, RGB(red, green, blue));
            }
        }
    }
}

static const WCHAR *rule_help[RULE_COUNT] = {
    L"只删除每行开头的普通空格，不处理 Tab。",
    L"只删除每行结尾的普通空格，不处理 Tab。",
    L"把连续的普通空格合成一个；不改变换行。",
    L"删除所有普通空格，英文词语也会连在一起。",
    L"把全角空格 U+3000 转成普通空格。",
    L"把不换行空格 U+00A0 转成普通空格。",
    L"每个 Tab 转成一个空格，与“删除 Tab”互斥。",
    L"直接删除 Tab，与“Tab 转一个普通空格”互斥。",
    L"删除正文之前的空白行；只有普通空格或 Tab 的行也算空行。",
    L"删除正文之后的空白行，包括结尾换行。",
    L"多条相邻空白行只保留一条，保留正常分段。",
    L"移除空白行，保留正文行之间的换行。",
    L"每个换行转成一个空格；CRLF 作为一个换行处理。",
    L"删除换行，直接连接前后文字；与“换行转空格”互斥。",
    L"只删除 U+200B，不删除表情使用的零宽连接符。",
    L"只删除 U+FEFF，不改变其他 Unicode 字符。",
    L"有文字时移除附带图片；纯图片保持不变。",
    L"有文字时移除文件附件，不删除磁盘文件；纯文件保持不变。"
};

static INT_PTR CALLBACK special_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    int i;
    (void)lparam;
    switch (message) {
    case WM_INITDIALOG: {
        TOOLINFOW tool;
        special_window = dialog;
        SendMessageW(dialog, WM_SETICON, ICON_BIG, (LPARAM)active_icon);
        SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)active_small_icon);
        rule_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, dialog, NULL, instance, NULL);
        if (rule_tooltip) SendMessageW(rule_tooltip, TTM_SETMAXTIPWIDTH, 0, 360);
        for (i = 0; i < RULE_COUNT; ++i) {
            CheckDlgButton(dialog, IDC_RULE_FIRST + i, (special_rules & RULE_BIT(i)) ? BST_CHECKED : BST_UNCHECKED);
            if (!rule_tooltip) continue;
            ZeroMemory(&tool, sizeof(tool));
            tool.cbSize = sizeof(tool);
            tool.hwnd = dialog;
            tool.uId = (UINT_PTR)GetDlgItem(dialog, IDC_RULE_FIRST + i);
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.lpszText = (WCHAR *)rule_help[i];
            SendMessageW(rule_tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) >= IDC_RULE_FIRST && LOWORD(wparam) <= IDC_RULE_LAST) {
            DWORD next = special_rules;
            LSTATUS result;
            int rule = LOWORD(wparam) - IDC_RULE_FIRST;
            if (IsDlgButtonChecked(dialog, LOWORD(wparam)) == BST_CHECKED) {
                next |= RULE_BIT(rule);
                if (rule == RULE_TAB_SPACE) next &= ~RULE_BIT(RULE_TAB_DELETE);
                if (rule == RULE_TAB_DELETE) next &= ~RULE_BIT(RULE_TAB_SPACE);
                if (rule == RULE_NEWLINE_SPACE) next &= ~RULE_BIT(RULE_NEWLINE_DELETE);
                if (rule == RULE_NEWLINE_DELETE) next &= ~RULE_BIT(RULE_NEWLINE_SPACE);
            } else next &= ~RULE_BIT(rule);
            result = save_option(L"SpecialRules", next);
            if (result == ERROR_SUCCESS) {
                special_rules = next;
                KillTimer(main_window, TIMER_CLIPBOARD);
                retries = 0;
            }
            for (i = 0; i < RULE_COUNT; ++i)
                CheckDlgButton(dialog, IDC_RULE_FIRST + i, (special_rules & RULE_BIT(i)) ? BST_CHECKED : BST_UNCHECKED);
            if (result != ERROR_SUCCESS) report_error(dialog, L"无法保存设置，已恢复原选项。", result);
            return TRUE;
        }
        if (LOWORD(wparam) == IDOK) return TRUE;
        if (LOWORD(wparam) != IDCANCEL) break;
        /* fall through */
    case WM_CLOSE:
        DestroyWindow(dialog);
        return TRUE;
    case WM_DESTROY:
        if (rule_tooltip) { DestroyWindow(rule_tooltip); rule_tooltip = NULL; }
        special_window = NULL;
        if (IsWindow(settings_window)) { EnableWindow(settings_window, TRUE); SetActiveWindow(settings_window); }
        return TRUE;
    }
    return FALSE;
}

static void show_special(void)
{
    if (!special_window) {
        special_window = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_SPECIAL), settings_window, special_proc, 0);
        if (!special_window) { report_error(settings_window, L"无法打开特殊清理配置。", GetLastError()); return; }
        EnableWindow(settings_window, FALSE);
    }
    ShowWindow(special_window, SW_SHOWNORMAL);
    SetForegroundWindow(special_window);
}

static INT_PTR CALLBACK settings_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_INITDIALOG: {
        TOOLINFOW tool;
        LOGFONTW font;
        HFONT dialog_font;
        settings_window = dialog;
        SendMessageW(dialog, WM_SETICON, ICON_BIG, (LPARAM)active_icon);
        SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)active_small_icon);
        CheckDlgButton(dialog, IDC_ENABLED, enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_STARTUP, startup_enabled() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_SPECIAL_ENABLED, special_enabled ? BST_CHECKED : BST_UNCHECKED);
        dialog_font = (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0);
        if (dialog_font && GetObjectW(dialog_font, sizeof(font), &font)) {
            LOGFONTW symbol = font;
            symbol.lfHeight = MulDiv(font.lfHeight, 11, 10);
            StringCchCopyW(symbol.lfFaceName, LF_FACESIZE, L"Segoe MDL2 Assets");
            wrench_font = CreateFontIndirectW(&symbol);
            font.lfHeight = MulDiv(font.lfHeight, 9, 10);
            status_font = CreateFontIndirectW(&font);
            if (status_font) SendDlgItemMessageW(dialog, IDC_STATUS_TEXT, WM_SETFONT, (WPARAM)status_font, TRUE);
        }
        status_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                                         WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                         CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                         dialog, NULL, instance, NULL);
        if (status_tooltip) {
            ZeroMemory(&tool, sizeof(tool));
            tool.cbSize = sizeof(tool);
            tool.hwnd = dialog;
            tool.uId = (UINT_PTR)GetDlgItem(dialog, IDC_STATUS);
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.lpszText = L"";
            SendMessageW(status_tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool);
            tool.uId = (UINT_PTR)GetDlgItem(dialog, IDC_SPECIAL_CONFIG);
            tool.lpszText = L"特殊清理配置";
            SendMessageW(status_tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool);
        }
        refresh_settings_status();
        return TRUE;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *item = (const DRAWITEMSTRUCT *)lparam;
        if (item && item->CtlID == IDC_SPECIAL_CONFIG) {
            RECT rect = item->rcItem;
            HGDIOBJ old_font = SelectObject(item->hDC, wrench_font ? wrench_font : GetStockObject(DEFAULT_GUI_FONT));
            BOOL keyboard_focus = (item->itemState & ODS_FOCUS) && !(item->itemState & ODS_NOFOCUSRECT);
            FillRect(item->hDC, &rect, GetSysColorBrush(COLOR_3DFACE));
            SetBkMode(item->hDC, TRANSPARENT);
            SetTextColor(item->hDC, GetSysColor((item->itemState & ODS_DISABLED) ? COLOR_GRAYTEXT :
                ((keyboard_focus || (item->itemState & ODS_SELECTED)) ? COLOR_HIGHLIGHT : COLOR_BTNTEXT)));
            DrawTextW(item->hDC, L"\xE90F", 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(item->hDC, old_font);
            return TRUE;
        }
        if (item && item->CtlID == IDC_STATUS) {
            RECT rect = item->rcItem;
            int width = rect.right - rect.left, height = rect.bottom - rect.top;
            int diameter = width < height ? width : height;
            COLORREF background = GetSysColor(COLOR_3DFACE);
            FillRect(item->hDC, &rect, GetSysColorBrush(COLOR_3DFACE));
            rect.left += (width - diameter) / 2;
            rect.top += (height - diameter) / 2;
            draw_status_dot(item->hDC, rect.left, rect.top, diameter, background);
            return TRUE;
        }
        break;
    }
    case WM_SETCURSOR:
        if ((HWND)wparam == GetDlgItem(dialog, IDC_SPECIAL_CONFIG) && LOWORD(lparam) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lparam == GetDlgItem(dialog, IDC_STATUS_TEXT)) {
            SetTextColor((HDC)wparam, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor((HDC)wparam, GetSysColor(COLOR_3DFACE));
            return (INT_PTR)GetSysColorBrush(COLOR_3DFACE);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_SPECIAL_CONFIG && HIWORD(wparam) == BN_CLICKED) { show_special(); return TRUE; }
        if (HIWORD(wparam) == BN_CLICKED &&
            (LOWORD(wparam) == IDC_ENABLED || LOWORD(wparam) == IDC_SPECIAL_ENABLED || LOWORD(wparam) == IDC_STARTUP)) {
            int control = LOWORD(wparam);
            BOOL value = IsDlgButtonChecked(dialog, control) == BST_CHECKED;
            BOOL previous = control == IDC_ENABLED ? enabled :
                            control == IDC_SPECIAL_ENABLED ? special_enabled : startup_enabled();
            LSTATUS result;
            if (value == previous) return TRUE;
            if (control == IDC_ENABLED) result = save_enabled(value);
            else if (control == IDC_SPECIAL_ENABLED) result = save_option(L"SpecialEnabled", value);
            else result = set_startup(value);
            if (result != ERROR_SUCCESS) {
                CheckDlgButton(dialog, control, previous ? BST_CHECKED : BST_UNCHECKED);
                report_error(dialog, L"无法保存设置，已恢复原选项。", result);
                return TRUE;
            }
            if (control == IDC_ENABLED) apply_enabled(value);
            else if (control == IDC_SPECIAL_ENABLED) apply_special_enabled(value);
            return TRUE;
        }
        if (LOWORD(wparam) == IDOK) return TRUE;
        if (LOWORD(wparam) == IDCANCEL) { DestroyWindow(dialog); return TRUE; }
        break;
    case WM_CLOSE:
        DestroyWindow(dialog);
        return TRUE;
    case WM_DESTROY:
        if (special_window) DestroyWindow(special_window);
        settings_window = NULL;
        if (status_tooltip) { DestroyWindow(status_tooltip); status_tooltip = NULL; }
        if (status_font) { DeleteObject(status_font); status_font = NULL; }
        if (wrench_font) { DeleteObject(wrench_font); wrench_font = NULL; }
        return TRUE;
    }
    return FALSE;
}

static void show_settings(void)
{
    if (special_window) { ShowWindow(special_window, SW_SHOWNORMAL); SetForegroundWindow(special_window); return; }
    if (!settings_window) {
        settings_window = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, settings_proc, 0);
        if (!settings_window) { report_error(NULL, L"无法打开设置窗口。", GetLastError()); return; }
    }
    ShowWindow(settings_window, SW_SHOWNORMAL);
    /* An explicit --settings / second-launch request should also work when
       the launching process supplied STARTF_USESHOWWINDOW with SW_HIDE. */
    if (!IsWindowVisible(settings_window))
        SetWindowPos(settings_window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(settings_window);
}

static HMENU create_tray_menu(void)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return NULL;
    AppendMenuW(menu, MF_OWNERDRAW | MF_GRAYED, IDM_STATUS, enabled ? L"监听中" : L"空闲");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (enabled ? MF_CHECKED : MF_UNCHECKED), IDM_TOGGLE, L"开启监听");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"设置");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");
    SetMenuDefaultItem(menu, IDM_SETTINGS, FALSE);
    return menu;
}

static BOOL is_menu_toggle(UINT command)
{
    return command == IDM_TOGGLE;
}

static void refresh_active_menu(void)
{
    RECT rect;
    MENUITEMINFOW status;
    if (!active_menu) return;
    CheckMenuItem(active_menu, IDM_TOGGLE, MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
    ZeroMemory(&status, sizeof(status));
    status.cbSize = sizeof(status);
    status.fMask = MIIM_DATA;
    status.dwItemData = (ULONG_PTR)(enabled ? L"监听中" : L"空闲");
    SetMenuItemInfoW(active_menu, IDM_STATUS, FALSE, &status);
    if (GetMenuItemRect(main_window, active_menu, 0, &rect)) {
        POINT center = {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
        HWND popup = WindowFromPoint(center);
        if (popup) RedrawWindow(popup, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
}

/* Only filter this thread's native menu loop, and only consume activation of
   checkable options. Outside clicks, Escape, Settings and Exit stay native. */
static LRESULT CALLBACK menu_filter(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == MSGF_MENU && active_menu) {
        const MSG *message = (const MSG *)lparam;
        UINT command = 0;
        if (message->message == WM_KEYDOWN && message->wParam == VK_ESCAPE) {
            EndMenu();
            return 1;
        }
        if (message->message == WM_LBUTTONUP || message->message == WM_RBUTTONUP) {
            int position = MenuItemFromPoint(main_window, active_menu, message->pt);
            if (position >= 0) command = GetMenuItemID(active_menu, position);
        } else if (message->message == WM_KEYDOWN &&
                   (message->wParam == VK_RETURN || message->wParam == VK_SPACE)) {
            int position, count = GetMenuItemCount(active_menu);
            for (position = 0; position < count; ++position) {
                if (GetMenuState(active_menu, position, MF_BYPOSITION) & MF_HILITE) {
                    command = GetMenuItemID(active_menu, position);
                    break;
                }
            }
            if (is_menu_toggle(command) && (message->lParam & (1L << 30))) return 1;
        }
        if (is_menu_toggle(command)) {
            SendMessageW(main_window, WM_COMMAND, command, 0);
            refresh_active_menu();
            return 1;
        }
    }
    return CallNextHookEx(NULL, code, wparam, lparam);
}

static void show_menu(POINT point)
{
    NONCLIENTMETRICSW metrics;
    HMENU menu;
    HHOOK filter;
    UINT choice;
    if (active_menu) return;
    ZeroMemory(&metrics, sizeof(metrics));
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        menu_status_font = CreateFontIndirectW(&metrics.lfMenuFont);
    menu = create_tray_menu();
    if (!menu) {
        if (menu_status_font) { DeleteObject(menu_status_font); menu_status_font = NULL; }
        return;
    }
    active_menu = menu;
    filter = SetWindowsHookExW(WH_MSGFILTER, menu_filter, NULL, GetCurrentThreadId());
    SetForegroundWindow(main_window);
    do {
        choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, main_window, NULL);
        /* If a native accessibility action bypasses the filter (or the hook
           cannot be installed), still keep checkable choices in this menu. */
        if (is_menu_toggle(choice)) {
            SendMessageW(main_window, WM_COMMAND, choice, 0);
            refresh_active_menu();
        }
    } while (is_menu_toggle(choice));
    if (filter) UnhookWindowsHookEx(filter);
    active_menu = NULL;
    PostMessageW(main_window, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (menu_status_font) { DeleteObject(menu_status_font); menu_status_font = NULL; }
    if (choice) PostMessageW(main_window, WM_COMMAND, choice, 0);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (taskbar_created && message == taskbar_created) {
        tray_added = FALSE;
        update_tray(TRUE);
        return 0;
    }
    switch (message) {
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *item = (MEASUREITEMSTRUCT *)lparam;
        if (item && item->CtlType == ODT_MENU && item->itemID == IDM_STATUS) {
            HDC dc = GetDC(window);
            HGDIOBJ previous = SelectObject(dc, menu_status_font ? menu_status_font : GetStockObject(DEFAULT_GUI_FONT));
            SIZE extent;
            GetTextExtentPoint32W(dc, L"监听中", 3, &extent);
            item->itemWidth = extent.cx + GetSystemMetrics(SM_CXMENUCHECK) + 24;
            item->itemHeight = extent.cy + 10;
            SelectObject(dc, previous);
            ReleaseDC(window, dc);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *item = (const DRAWITEMSTRUCT *)lparam;
        if (item && item->CtlType == ODT_MENU && item->itemID == IDM_STATUS) {
            RECT rect = item->rcItem;
            int saved = SaveDC(item->hDC);
            int gutter = GetSystemMetrics(SM_CXMENUCHECK) + 8;
            int diameter = MulDiv(rect.bottom - rect.top, 1, 3);
            FillRect(item->hDC, &rect, GetSysColorBrush(COLOR_MENU));
            draw_status_dot(item->hDC, rect.left + (gutter - diameter) / 2,
                            rect.top + (rect.bottom - rect.top - diameter) / 2,
                            diameter, GetSysColor(COLOR_MENU));
            SelectObject(item->hDC, menu_status_font ? menu_status_font : GetStockObject(DEFAULT_GUI_FONT));
            SetBkMode(item->hDC, TRANSPARENT);
            SetTextColor(item->hDC, GetSysColor(COLOR_GRAYTEXT));
            rect.left += gutter;
            DrawTextW(item->hDC, enabled ? L"监听中" : L"空闲", -1, &rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RestoreDC(item->hDC, saved);
            return TRUE;
        }
        break;
    }
    case WM_CLIPBOARDUPDATE:
        if (enabled) {
            pending_sequence = GetClipboardSequenceNumber();
            retries = 0;
            clean_clipboard();
        }
        return 0;
    case WM_TIMER:
        if (wparam == TIMER_CLIPBOARD) clean_clipboard();
        else if (wparam == TIMER_TRAY) update_tray(TRUE);
        return 0;
    case WM_TRAY:
        if (LOWORD(lparam) == WM_CONTEXTMENU) {
            POINT point = {(SHORT)LOWORD(wparam), (SHORT)HIWORD(wparam)};
            if (point.x == -1 && point.y == -1) GetCursorPos(&point);
            show_menu(point);
        } else if (LOWORD(lparam) == WM_LBUTTONDBLCLK || LOWORD(lparam) == NIN_KEYSELECT) show_settings();
        return 0;
    case WM_SETTINGS:
        show_settings();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDM_TOGGLE) {
            LSTATUS result = save_enabled(!enabled);
            if (result != ERROR_SUCCESS) report_error(settings_window, L"无法保存清洗状态。", result);
            else {
                apply_enabled(!enabled);
                if (settings_window) CheckDlgButton(settings_window, IDC_ENABLED, enabled ? BST_CHECKED : BST_UNCHECKED);
            }
        } else if (LOWORD(wparam) == IDM_SETTINGS) show_settings();
        else if (LOWORD(wparam) == IDM_EXIT) DestroyWindow(window);
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wparam) DestroyWindow(window);
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY: {
        NOTIFYICONDATAW data;
        KillTimer(window, TIMER_CLIPBOARD);
        KillTimer(window, TIMER_TRAY);
        if (listening) RemoveClipboardFormatListener(window);
        if (settings_window) DestroyWindow(settings_window);
        ZeroMemory(&data, sizeof(data));
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &data);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE module, HINSTANCE previous, LPWSTR command_line, int show)
{
    HANDLE mutex;
    WNDCLASSEXW window_class;
    MSG message;
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_STANDARD_CLASSES };
    BOOL open_settings = FALSE;
    int argc, index, exit_code = 0;
    LPWSTR *argv;
    (void)previous; (void)command_line; (void)show;
    instance = module;
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (index = 1; index < argc; ++index) if (!lstrcmpW(argv[index], L"--settings")) open_settings = TRUE;
        LocalFree(argv);
    }
    mutex = CreateMutexW(NULL, TRUE, APP_MUTEX);
    if (!mutex) { report_error(NULL, L"无法启动 Clear Copy。", GetLastError()); return 1; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = NULL;
        /* The first instance may still be registering its window. */
        for (index = 0; index < 40 && !existing; ++index) {
            existing = FindWindowW(APP_CLASS, NULL);
            if (!existing) Sleep(25);
        }
        if (existing) {
            DWORD pid;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(existing, WM_SETTINGS, 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }
    InitCommonControlsEx(&controls);
    load_config();
    active_icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    active_small_icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    paused_icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_PAUSED), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    html_format = RegisterClipboardFormatW(L"HTML Format");
    rtf_format = RegisterClipboardFormatW(L"Rich Text Format");
    shell_idlist_format = RegisterClipboardFormatW(L"Shell IDList Array");
    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    ZeroMemory(&window_class, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = APP_CLASS;
    window_class.hIcon = active_icon;
    window_class.hIconSm = active_small_icon;
    if (!RegisterClassExW(&window_class)) {
        report_error(NULL, L"无法注册后台窗口。", GetLastError());
        CloseHandle(mutex);
        return 1;
    }
    /* Hidden top-level window also receives Explorer's TaskbarCreated broadcast. */
    main_window = CreateWindowExW(WS_EX_TOOLWINDOW, APP_CLASS, L"Clear Copy", WS_POPUP,
                                  0, 0, 0, 0, NULL, NULL, instance, NULL);
    if (!main_window) { report_error(NULL, L"无法创建后台窗口。", GetLastError()); CloseHandle(mutex); return 1; }
    listening = AddClipboardFormatListener(main_window);
    if (!listening) {
        report_error(NULL, L"无法监听剪贴板。", GetLastError());
        DestroyWindow(main_window);
        CloseHandle(mutex);
        return 1;
    }
    update_tray(TRUE);
    if (!tray_added) show_settings();
    else if (open_settings) show_settings();
    while ((index = GetMessageW(&message, NULL, 0, 0)) > 0) {
        if ((!special_window || !IsDialogMessageW(special_window, &message)) &&
            (!settings_window || !IsDialogMessageW(settings_window, &message))) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (index == -1) exit_code = 1;
    CloseHandle(mutex);
    return exit_code;
}
