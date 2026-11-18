/* Test-only entry point. Load the exact production code/resources normally,
   then switch to the isolated station before creating any application window.
   Some Windows environments block DLL initialization if a new process starts
   directly on a noninteractive station. This adapter avoids that limitation. */
#define _WIN32_WINNT 0x0601
#include <wchar.h>
static wchar_t test_run_key[256];
#define RUN_KEY test_run_key
#define wWinMain clear_copy_main
#include "../src/main.c"
#undef wWinMain

int WINAPI wWinMain(HINSTANCE module, HINSTANCE previous, LPWSTR command, int show)
{
    WCHAR name[128];
    HWINSTA station;
    HDESK desktop;
    if (!GetEnvironmentVariableW(L"CLEAR_COPY_TEST_STATION", name, ARRAYSIZE(name))) return 91;
    StringCchPrintfW(test_run_key, ARRAYSIZE(test_run_key), L"Software\\ClearCopy\\%ls", name);
    station = OpenWindowStationW(name, FALSE, WINSTA_ALL_ACCESS);
    if (!station || !SetProcessWindowStation(station)) return 92;
    desktop = OpenDesktopW(L"Default", 0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
                           DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_ENUMERATE);
    if (!desktop || !SetThreadDesktop(desktop)) return 93;
    /* Check real native menu structure in both states before the message loop.
       Startup writes are redirected to this run's isolated test registry key. */
    for (int state = 0; state < 2; ++state) {
        WCHAR label[64];
        HMENU menu;
        UINT flags;
        enabled = state;
        special_enabled = state;
        menu = create_tray_menu();
        if (!menu || GetMenuItemCount(menu) != 6 || GetMenuItemID(menu, 0) != IDM_STATUS) return 94;
        flags = GetMenuState(menu, IDM_STATUS, MF_BYCOMMAND);
        if (!(flags & MF_GRAYED) || !(flags & MF_OWNERDRAW)) return 95;
        if (!!(GetMenuState(menu, IDM_TOGGLE, MF_BYCOMMAND) & MF_CHECKED) != state) return 96;
        GetMenuStringW(menu, IDM_TOGGLE, label, ARRAYSIZE(label), MF_BYCOMMAND);
        if (lstrcmpW(label, L"开启监听")) return 97;
        if (GetMenuItemID(menu, 4) != IDM_SETTINGS || GetMenuItemID(menu, 5) != IDM_EXIT) return 98;
        DestroyMenu(menu);
    }
    special_enabled = FALSE;
    return clear_copy_main(module, previous, command, show);
}
