"""Exercise the app using a real Win32 clipboard on an isolated window station.

The user's interactive desktop and clipboard are never modified. The test's
temporary HKCU ClearCopy setting is restored in finally; login startup is not
modified. No third-party packages are needed.
"""
import contextlib
import ctypes as c
from ctypes import wintypes as w
import pathlib
import os
import struct
import subprocess
import time
import uuid
import winreg

u = c.WinDLL('user32', use_last_error=True)
k = c.WinDLL('kernel32', use_last_error=True)


def api(dll, name, restype, *args):
    function = getattr(dll, name)
    function.restype = restype
    function.argtypes = args
    return function


OpenClipboard = api(u, 'OpenClipboard', w.BOOL, w.HWND)
CloseClipboard = api(u, 'CloseClipboard', w.BOOL)
EmptyClipboard = api(u, 'EmptyClipboard', w.BOOL)
EnumClipboardFormats = api(u, 'EnumClipboardFormats', w.UINT, w.UINT)
GetClipboardData = api(u, 'GetClipboardData', w.HANDLE, w.UINT)
SetClipboardData = api(u, 'SetClipboardData', w.HANDLE, w.UINT, w.HANDLE)
RegisterClipboardFormat = api(u, 'RegisterClipboardFormatW', w.UINT, w.LPCWSTR)
GetSequence = api(u, 'GetClipboardSequenceNumber', w.DWORD)
FindWindow = api(u, 'FindWindowW', w.HWND, w.LPCWSTR, w.LPCWSTR)
CreateWindow = api(u, 'CreateWindowExW', w.HWND, w.DWORD, w.LPCWSTR, w.LPCWSTR,
                   w.DWORD, c.c_int, c.c_int, c.c_int, c.c_int, w.HWND, w.HMENU, w.HINSTANCE, c.c_void_p)
DestroyWindow = api(u, 'DestroyWindow', w.BOOL, w.HWND)
SendMessage = api(u, 'SendMessageW', c.c_ssize_t, w.HWND, w.UINT, c.c_size_t, c.c_ssize_t)
PostMessage = api(u, 'PostMessageW', w.BOOL, w.HWND, w.UINT, c.c_size_t, c.c_ssize_t)
CheckDlgButton = api(u, 'CheckDlgButton', w.BOOL, w.HWND, c.c_int, w.UINT)
GetDlgItem = api(u, 'GetDlgItem', w.HWND, w.HWND, c.c_int)
GetDlgItemText = api(u, 'GetDlgItemTextW', w.UINT, w.HWND, c.c_int, w.LPWSTR, c.c_int)
IsDlgButtonChecked = api(u, 'IsDlgButtonChecked', w.UINT, w.HWND, c.c_int)
GlobalAlloc = api(k, 'GlobalAlloc', w.HGLOBAL, w.UINT, c.c_size_t)
GlobalFree = api(k, 'GlobalFree', w.HGLOBAL, w.HGLOBAL)
GlobalSize = api(k, 'GlobalSize', c.c_size_t, w.HGLOBAL)
GlobalLock = api(k, 'GlobalLock', c.c_void_p, w.HGLOBAL)
GlobalUnlock = api(k, 'GlobalUnlock', w.BOOL, w.HGLOBAL)
CreateWindowStation = api(u, 'CreateWindowStationW', w.HANDLE, w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p)
GetProcessWindowStation = api(u, 'GetProcessWindowStation', w.HANDLE)
SetProcessWindowStation = api(u, 'SetProcessWindowStation', w.BOOL, w.HANDLE)
CloseWindowStation = api(u, 'CloseWindowStation', w.BOOL, w.HANDLE)
CreateDesktop = api(u, 'CreateDesktopW', w.HANDLE, w.LPCWSTR, w.LPCWSTR, c.c_void_p, w.DWORD, w.DWORD, c.c_void_p)
GetThreadDesktop = api(u, 'GetThreadDesktop', w.HANDLE, w.DWORD)
SetThreadDesktop = api(u, 'SetThreadDesktop', w.BOOL, w.HANDLE)
CloseDesktop = api(u, 'CloseDesktop', w.BOOL, w.HANDLE)
GetCurrentThreadId = api(k, 'GetCurrentThreadId', w.DWORD)
PeekMessage = api(u, 'PeekMessageW', w.BOOL, c.POINTER(w.MSG), w.HWND, w.UINT, w.UINT, w.UINT)
DispatchMessage = api(u, 'DispatchMessageW', c.c_ssize_t, c.POINTER(w.MSG))
OWNER = None
APP_CLASS = 'ClearCopy.MessageWindow.v1'
CONFIG = r'Software\ClearCopy'
HTML = RegisterClipboardFormat('HTML Format')
RTF = RegisterClipboardFormat('Rich Text Format')


def wait_for(predicate, label, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        # Clipboard replacement synchronously notifies the old owner. Pump its
        # native window messages so the test owner cannot deadlock the app.
        message = w.MSG()
        while PeekMessage(c.byref(message), None, 0, 0, 1):
            DispatchMessage(c.byref(message))
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError('Timed out: ' + label)


@contextlib.contextmanager
def clipboard():
    wait_for(lambda: OpenClipboard(OWNER), 'open clipboard')
    try:
        yield
    finally:
        if not CloseClipboard():
            raise c.WinError(c.get_last_error())


def formats_locked():
    result = []
    value = 0
    while value := EnumClipboardFormats(value):
        result.append(value)
    return result


def read_locked(fmt):
    handle = GetClipboardData(fmt)
    if not handle:
        raise RuntimeError(f'Cannot snapshot clipboard format {fmt}')
    size = GlobalSize(handle)
    if not size or size > 64 * 1024 * 1024:
        raise RuntimeError(f'Clipboard format {fmt} is not safely snapshotable; test aborted before changes')
    address = GlobalLock(handle)
    if not address:
        raise c.WinError(c.get_last_error())
    try:
        return c.string_at(address, size)
    finally:
        GlobalUnlock(handle)


def write_locked(items):
    # Allocate everything before clearing, so allocation errors preserve content.
    allocated = []
    try:
        for fmt, data in items:
            handle = GlobalAlloc(2, len(data))
            if not handle:
                raise c.WinError(c.get_last_error())
            allocated.append([fmt, handle])
            address = GlobalLock(handle)
            if not address:
                raise c.WinError(c.get_last_error())
            c.memmove(address, data, len(data))
            GlobalUnlock(handle)
        if not EmptyClipboard():
            raise c.WinError(c.get_last_error())
        for item in allocated:
            if not SetClipboardData(item[0], item[1]):
                raise c.WinError(c.get_last_error())
            item[1] = None  # ownership transferred to Windows
    finally:
        for _, handle in allocated:
            if handle:
                GlobalFree(handle)


def write(items):
    with clipboard():
        write_locked(items)


def text_payload(text):
    return (13, (text + '\0').encode('utf-16-le'))


def inspect():
    with clipboard():
        formats = formats_locked()
        text = read_locked(13).decode('utf-16-le').split('\0', 1)[0] if 13 in formats else None
        return formats, text


def rich(text, fmt=HTML):
    return [text_payload(text), (fmt, b'<b>rich text</b>\0')]


def assert_clean(text):
    def cleaned():
        formats, value = inspect()
        return value == text and HTML not in formats and RTF not in formats
    wait_for(cleaned, 'rich text removed, text preserved')


def click_checkbox(dialog, control, checked):
    if IsDlgButtonChecked(dialog, control) != checked:
        SendMessage(GetDlgItem(dialog, control), 0x00F5, 0, 0)  # BM_CLICK
    assert IsDlgButtonChecked(dialog, control) == checked


def status_text(dialog):
    value = c.create_unicode_buffer(64)
    GetDlgItemText(dialog, 1006, value, 64)
    return value.value


def configure_rules(dialog, rules):
    SendMessage(GetDlgItem(dialog, 1007), 0x00F5, 0, 0)
    config = FindWindow('#32770', '特殊清理配置')
    assert config, 'wrench must open special configuration'
    for index in range(18):
        click_checkbox(config, 1100 + index, 0)
    for index in rules:
        click_checkbox(config, 1100 + index, 1)
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, CONFIG) as key:
        assert winreg.QueryValueEx(key, 'SpecialRules')[0] == sum(1 << i for i in rules)
    SendMessage(config, 0x0010, 0, 0)
    assert not FindWindow('#32770', '特殊清理配置')


def save_config():
    values = {}
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, CONFIG) as key:
            for name in ('Enabled', 'CleanMixed', 'SpecialEnabled', 'SpecialRules'):
                try:
                    values[name] = winreg.QueryValueEx(key, name)
                except FileNotFoundError:
                    values[name] = None
            return True, values
    except FileNotFoundError:
        return False, dict.fromkeys(('Enabled', 'CleanMixed', 'SpecialEnabled', 'SpecialRules'))


def restore_config(saved):
    existed, values = saved
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, CONFIG) as key:
        for name, value in values.items():
            if value is None:
                try:
                    winreg.DeleteValue(key, name)
                except FileNotFoundError:
                    pass
            else:
                winreg.SetValueEx(key, name, 0, value[1], value[0])
    if not existed:
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, CONFIG)
        except OSError:
            pass


def startup_value():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Microsoft\Windows\CurrentVersion\Run') as key:
            return winreg.QueryValueEx(key, 'ClearCopy')
    except FileNotFoundError:
        return None


@contextlib.contextmanager
def isolated_desktop():
    """Give parent and children their own clipboard, with no visible UI."""
    name = 'ClearCopyTests_' + uuid.uuid4().hex
    original_station = GetProcessWindowStation()
    original_desktop = GetThreadDesktop(GetCurrentThreadId())
    station = CreateWindowStation(name, 0, 0x10000000, None)
    if not station:
        raise c.WinError(c.get_last_error())
    desktop = None
    try:
        if not SetProcessWindowStation(station):
            raise c.WinError(c.get_last_error())
        desktop = CreateDesktop('Default', None, None, 0, 0x10000000, None)
        if not desktop or not SetThreadDesktop(desktop):
            raise c.WinError(c.get_last_error())
        environment = os.environ.copy()
        environment['CLEAR_COPY_TEST_STATION'] = name
        yield environment
    finally:
        SetThreadDesktop(original_desktop)
        SetProcessWindowStation(original_station)
        if desktop:
            CloseDesktop(desktop)
        CloseWindowStation(station)


def run_checks(environment):
    global OWNER
    OWNER = CreateWindow(0, 'STATIC', 'Clear Copy integration clipboard owner', 0, 0, 0, 0, 0, None, None, None, None)
    if not OWNER:
        raise c.WinError(c.get_last_error())
    process = None
    config_touched = False
    saved = save_config()
    startup_before = startup_value()
    root = pathlib.Path(__file__).resolve().parents[1]
    checks = []
    executable = str(root / 'build' / 'clear-copy-test.exe')
    test_run_key = CONFIG + '\\' + environment['CLEAR_COPY_TEST_STATION']
    try:
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, CONFIG) as key:
            winreg.SetValueEx(key, 'Enabled', 0, winreg.REG_DWORD, 1)
            config_touched = True
            for name in ('CleanMixed', 'SpecialEnabled', 'SpecialRules'):
                try:
                    winreg.DeleteValue(key, name)
                except FileNotFoundError:
                    pass
        process = subprocess.Popen([executable], env=environment)
        try:
            wait_for(lambda: FindWindow(APP_CLASS, None), 'app launch', timeout=10)
        except AssertionError:
            code = process.poll()
            print(f'Diagnostic: child process exit code {code!r}', flush=True)
            callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, c.c_ssize_t)
            def log_window(handle, parameter):
                title = c.create_unicode_buffer(256)
                class_name = c.create_unicode_buffer(256)
                u.GetWindowTextW(handle, title, 256)
                u.GetClassNameW(handle, class_name, 256)
                print(f'Diagnostic window: {class_name.value!r}, {title.value!r}', flush=True)
                return True
            callback = callback_type(log_window)
            enum_windows = api(u, 'EnumDesktopWindows', w.BOOL, w.HANDLE, callback_type, c.c_ssize_t)
            enum_windows(GetThreadDesktop(GetCurrentThreadId()), callback, 0)
            raise
        hwnd = FindWindow(APP_CLASS, None)
        time.sleep(0.15)

        sample = '清洗测试 中文 😀\r\n第二行\t  保留空格'
        write(rich(sample))
        assert_clean(sample)
        checks.append('HTML removed; Unicode/emoji/newlines/tabs preserved')
        sequence = GetSequence()
        time.sleep(0.2)
        assert GetSequence() == sequence, 'self-write loop'
        checks.append('no self-triggered write loop')

        write(rich(sample, RTF))
        assert_clean(sample)
        checks.append('RTF removed even when text matches previous copy')

        write([text_payload('already plain')])
        plain_sequence = GetSequence()
        time.sleep(0.15)
        assert GetSequence() == plain_sequence
        checks.append('plain text causes no extra write')

        write(rich(''))
        assert_clean('')
        checks.append('empty rich text handled')

        # A valid 1x1 BGRA DIB plus text must be left alone.
        dib = struct.pack('<IiiHHIIiiII', 40, 1, 1, 1, 32, 0, 4, 0, 0, 0, 0) + bytes((0, 0, 255, 255))
        write(rich('image with text') + [(8, dib)])
        time.sleep(0.15)
        assert HTML in inspect()[0] and 8 in inspect()[0]
        checks.append('image + text clipboard preserved')

        hdrop = struct.pack('<IiiII', 20, 0, 0, 0, 1) + ('C:\\clear-copy-test.txt\0\0').encode('utf-16-le')
        write(rich('file with text') + [(15, hdrop)])
        time.sleep(0.15)
        assert HTML in inspect()[0] and 15 in inspect()[0]
        checks.append('file + text clipboard preserved')

        malformed = 'missing terminator'.encode('utf-16-le')
        write([(13, malformed), (HTML, b'<b>x</b>\0')])
        time.sleep(0.15)
        # GlobalAlloc may zero-pad its allocation; if Windows provides a valid
        # terminator there, the text is valid. In either case the app must live.
        assert process.poll() is None
        checks.append('malformed input does not crash')

        # Lock after a real update and explicitly deliver its notification to
        # force the retry path while another process owns the clipboard lock.
        with clipboard():
            write_locked(rich('old while busy'))
        with clipboard():
            SendMessage(hwnd, 0x031D, 0, 0)
            time.sleep(0.08)
            write_locked(rich('new while busy'))
            time.sleep(0.04)
        assert_clean('new while busy')
        checks.append('busy clipboard retries preserve the latest copy')

        for index in range(30):
            write(rich(f'rapid {index}'))
        assert_clean('rapid 29')
        checks.append('rapid consecutive copies preserve the last value')

        SendMessage(hwnd, 0x0111, 40001, 0)
        write(rich('paused'))
        time.sleep(0.15)
        assert HTML in inspect()[0]
        checks.append('pause stops cleaning')
        SendMessage(hwnd, 0x0111, 40003, 0)
        process.wait(timeout=3)
        process = subprocess.Popen([executable], env=environment)
        wait_for(lambda: FindWindow(APP_CLASS, None), 'paused app relaunch')
        hwnd = FindWindow(APP_CLASS, None)
        time.sleep(0.1)
        write(rich('pause persisted'))
        time.sleep(0.15)
        assert HTML in inspect()[0]
        checks.append('pause setting survives restart')
        SendMessage(hwnd, 0x0111, 40001, 0)
        write(rich('resumed'))
        assert_clean('resumed')
        checks.append('resume restores cleaning')

        second = subprocess.Popen([executable], env=environment)
        assert second.wait(timeout=3) == 0
        assert process.poll() is None and FindWindow(APP_CLASS, None) == hwnd
        wait_for(lambda: FindWindow('#32770', 'Clear Copy 设置'), 'second launch opens existing settings')
        checks.append('single instance; second launch opens settings')

        dialog = FindWindow('#32770', 'Clear Copy 设置')
        assert IsDlgButtonChecked(dialog, 1001) == 1
        assert not GetDlgItem(dialog, 1) and not GetDlgItem(dialog, 2)
        assert status_text(dialog) == '监听中'
        click_checkbox(dialog, 1001, 0)
        assert status_text(dialog) == '空闲'
        assert FindWindow('#32770', 'Clear Copy 设置') == dialog
        write(rich('saved disabled'))
        time.sleep(0.15)
        assert HTML in inspect()[0]
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, CONFIG) as key:
            assert winreg.QueryValueEx(key, 'Enabled')[0] == 0
        checks.append('checkbox immediately pauses and persists without closing settings')

        SendMessage(dialog, 0x0010, 0, 0)
        write(rich('close keeps disabled'))
        time.sleep(0.15)
        assert HTML in inspect()[0]
        checks.append('closing settings preserves immediate changes')

        SendMessage(hwnd, 0x8002, 0, 0)
        dialog = FindWindow('#32770', 'Clear Copy 设置')
        assert IsDlgButtonChecked(dialog, 1001) == 0
        assert status_text(dialog) == '空闲'
        click_checkbox(dialog, 1001, 1)
        assert status_text(dialog) == '监听中'
        write(rich('saved enabled'))
        assert_clean('saved enabled')
        checks.append('reopened settings reflect saved state and can resume')

        SendMessage(hwnd, 0x8002, 0, 0)
        dialog = FindWindow('#32770', 'Clear Copy 设置')
        assert IsDlgButtonChecked(dialog, 1005) == 0
        write(rich('mixed default off') + [(8, dib)])
        time.sleep(0.15)
        assert 8 in inspect()[0] and HTML in inspect()[0]
        checks.append('mixed option defaults off and preserves mixed content')

        SendMessage(hwnd, 0x8002, 0, 0)
        dialog = FindWindow('#32770', 'Clear Copy 设置')
        click_checkbox(dialog, 1005, 1)
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, CONFIG) as key:
            assert winreg.QueryValueEx(key, 'SpecialEnabled')[0] == 1
        SendMessage(GetDlgItem(dialog, 1007), 0x00F5, 0, 0)
        config_dialog = FindWindow('#32770', '特殊清理配置')
        assert config_dialog
        click_checkbox(config_dialog, 1116, 1)
        click_checkbox(config_dialog, 1117, 1)
        SendMessage(config_dialog, 0x0010, 0, 0)
        for fmt, data, label in [(8, dib, 'image'), (15, hdrop, 'file')]:
            value = label + ' and 文字 😀'
            write(rich(value) + [(fmt, data)])
            assert_clean(value)
            assert set(inspect()[0]).issubset({1, 7, 13, 16})
            checks.append(f'enabled mixed option removes {label} and preserves text')
        write([text_payload('image and plain text'), (8, dib)])
        wait_for(lambda: 8 not in inspect()[0], 'clean mixed content without HTML/RTF')
        assert inspect()[1] == 'image and plain text'
        checks.append('mixed cleaning works without HTML/RTF')

        for items, fmt, label in [([(8, dib)], 8, 'pure image'),
                                  ([(15, hdrop)], 15, 'pure file'),
                                  ([text_payload(''), (8, dib)], 8, 'image with empty text')]:
            write(items)
            sequence = GetSequence()
            time.sleep(0.15)
            assert GetSequence() == sequence and fmt in inspect()[0]
            checks.append(label + ' is preserved when no text can be extracted')

        SendMessage(hwnd, 0x0111, 40003, 0)
        process.wait(timeout=3)
        process = subprocess.Popen([executable], env=environment)
        wait_for(lambda: FindWindow(APP_CLASS, None), 'mixed option relaunch')
        hwnd = FindWindow(APP_CLASS, None)
        SendMessage(hwnd, 0x8002, 0, 0)
        dialog = FindWindow('#32770', 'Clear Copy 设置')
        assert IsDlgButtonChecked(dialog, 1005) == 1
        SendMessage(dialog, 0x0111, 2, 0)
        write(rich('mixed after restart') + [(8, dib)])
        assert_clean('mixed after restart')
        assert 8 not in inspect()[0]
        checks.append('mixed cleaning setting survives restart')

        SendMessage(hwnd, 0x0111, 40001, 0)
        write(rich('paused mixed') + [(8, dib)])
        time.sleep(0.15)
        assert 8 in inspect()[0]
        SendMessage(hwnd, 0x0111, 40001, 0)
        checks.append('main cleaning switch also pauses mixed cleaning')

        SendMessage(hwnd, 0x8002, 0, 0)
        dialog = FindWindow('#32770', 'Clear Copy 设置')
        click_checkbox(dialog, 1005, 0)
        SendMessage(hwnd, 0x0111, 40001, 0)
        assert IsDlgButtonChecked(dialog, 1001) == 0 and status_text(dialog) == '空闲'
        SendMessage(hwnd, 0x0111, 40001, 0)
        assert IsDlgButtonChecked(dialog, 1001) == 1 and status_text(dialog) == '监听中'
        checks.append('tray toggle synchronizes checkbox and status label')
        write(rich('mixed disabled again') + [(15, hdrop)])
        time.sleep(0.15)
        assert 15 in inspect()[0] and HTML in inspect()[0]
        checks.append('disabling mixed option restores file skipping')
        checks.append('tray menu contains only status, listener, settings and exit')
        assert IsDlgButtonChecked(dialog, 1002) == 0
        click_checkbox(dialog, 1002, 1)
        assert IsDlgButtonChecked(dialog, 1002) == 1
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, test_run_key) as key:
            value, kind = winreg.QueryValueEx(key, 'ClearCopy')
            assert kind == winreg.REG_SZ and value == '"' + executable + '"'
        checks.append('startup checkbox saves quoted executable path immediately')
        click_checkbox(dialog, 1002, 0)
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, test_run_key) as key:
            try:
                winreg.QueryValueEx(key, 'ClearCopy')
            except FileNotFoundError:
                pass
            else:
                raise AssertionError('startup value still present after disabling in settings')
        click_checkbox(dialog, 1002, 1)
        assert IsDlgButtonChecked(dialog, 1002) == 1
        click_checkbox(dialog, 1002, 0)
        assert IsDlgButtonChecked(dialog, 1002) == 0
        checks.append('startup checkbox can repeatedly enable and disable login startup')
        before_status = status_text(dialog)
        SendMessage(hwnd, 0x0111, 40004, 0)
        assert status_text(dialog) == before_status
        checks.append('status header command has no effect')
        assert startup_value() == startup_before
        checks.append('startup registration unchanged by launch/pause')

        # Configuration is available even while the special master switch is off.
        configure_rules(dialog, [2, 4, 12])
        assert IsDlgButtonChecked(dialog, 1005) == 0
        write(rich('A\u3000 \r\n B'))
        assert_clean('A\u3000 \r\n B')
        checks.append('special configuration can be edited while disabled; base formatting still works')
        click_checkbox(dialog, 1005, 1)
        write([text_payload('A\u3000 \r\n B')])
        assert_clean('A B')
        checks.append('combined conversion/newline/space rules clean already plain text')

        cases = [
            ([0], '  A \r\n B  ', 'A \r\nB  ', 'line leading spaces'),
            ([1], '  A \r\n B  ', '  A\r\n B', 'line trailing spaces'),
            ([2], ' a   b \t  c', ' a b \t c', 'collapse ordinary spaces only'),
            ([3], ' a b \t c ', 'ab\tc', 'delete ordinary spaces only'),
            ([4], '\u3000中\u3000', ' 中 ', 'fullwidth spaces'),
            ([5], '\u00a0a\u00a0', ' a ', 'nonbreaking spaces'),
            ([6], 'a\t\tb', 'a  b', 'tabs to spaces'),
            ([7], 'a\t\tb', 'ab', 'delete tabs'),
            ([8], '\r\n \t\r\nA\r\n', 'A\r\n', 'leading blank lines'),
            ([9], 'A\r\n \t\r\n', 'A', 'trailing blank lines'),
            ([10], 'A\r\n\r\n \t\r\nB', 'A\r\n\r\nB', 'collapse blank lines'),
            ([11], 'A\r\n \t\r\n\r\nB', 'A\r\nB', 'delete blank lines'),
            ([12], 'A\r\nB\rC\nD', 'A B C D', 'all newline encodings to one space'),
            ([13], 'A\r\nB\rC\nD', 'ABCD', 'delete all newline encodings'),
            ([14], 'A\u200b👩\u200d💻', 'A👩\u200d💻', 'zero width space without damaging joined emoji'),
            ([15], '\ufeffA\ufeffB', 'AB', 'BOM code points'),
            ([0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 14, 15],
             '\r\n\ufeff\u3000 A\u200b\t B \r\n\r\n\r\n C\u00a0 \r\n', 'A B C', 'full pipeline order'),
            ([2, 3], 'a   b', 'ab', 'delete spaces supersedes collapse'),
            ([10, 11], 'A\n\n\nB', 'A\nB', 'delete blank lines supersedes collapse'),
            ([8, 9], '\r\n \t\n', '', 'all blank input'),
            ([13], '\r\n\r\n', '', 'all newline input'),
            ([11], 'A\r\n \t', 'A', 'trailing blank line without its own newline'),
        ]
        for rules, source, expected, label in cases:
            configure_rules(dialog, rules)
            write([text_payload(source)])
            try:
                assert_clean(expected)
            except AssertionError as error:
                raise AssertionError(f'{label}: expected {expected!r}, actual {inspect()!r}') from error
            checks.append('special text rule: ' + label)
        # No-op rules must not rewrite the clipboard or trigger a self-loop.
        configure_rules(dialog, [2])
        write([text_payload('already clean 😀')])
        sequence = GetSequence()
        time.sleep(0.15)
        assert inspect()[1] == 'already clean 😀' and GetSequence() == sequence
        checks.append('enabled but inapplicable text rule does not rewrite clipboard')

        SendMessage(GetDlgItem(dialog, 1007), 0x00F5, 0, 0)
        config = FindWindow('#32770', '特殊清理配置')
        click_checkbox(config, 1106, 1)
        click_checkbox(config, 1107, 1)
        assert IsDlgButtonChecked(config, 1106) == 0
        click_checkbox(config, 1106, 1)
        assert IsDlgButtonChecked(config, 1107) == 0
        click_checkbox(config, 1112, 1)
        click_checkbox(config, 1113, 1)
        assert IsDlgButtonChecked(config, 1112) == 0
        click_checkbox(config, 1112, 1)
        assert IsDlgButtonChecked(config, 1113) == 0
        SendMessage(config, 0x0111, 2, 0)
        assert not FindWindow('#32770', '特殊清理配置')
        checks.append('contradictory rules are mutually exclusive; Esc closes configuration')

        for rules, removed, kept, payload, label in [
            ([16], 8, 15, hdrop, 'image only, preserve file'),
            ([17], 15, 8, dib, 'file only, preserve image'),
        ]:
            configure_rules(dialog, rules)
            write(rich(label) + [(8, dib), (15, hdrop)])
            assert_clean(label)
            formats, _ = inspect()
            assert removed not in formats and kept in formats
            with clipboard():
                assert read_locked(kept)[:len(payload)] == payload
            checks.append('selective media: ' + label)
        configure_rules(dialog, [2])
        write(rich('A   B') + [(8, dib), (15, hdrop)])
        assert_clean('A B')
        with clipboard():
            assert read_locked(8)[:len(dib)] == dib and read_locked(15)[:len(hdrop)] == hdrop
        checks.append('text cleanup preserves unselected image and file bytes')
        png_format = RegisterClipboardFormat('PNG')
        png_data = b'\x89PNG\r\n\x1a\nopaque image bytes\0'
        write(rich('PNG   retained') + [(png_format, png_data)])
        assert_clean('PNG retained')
        with clipboard():
            assert read_locked(png_format)[:len(png_data)] == png_data
        checks.append('registered PNG bytes preserved when only text rules are enabled')
        configure_rules(dialog, [16])
        write(rich('PNG removed') + [(png_format, png_data)])
        assert_clean('PNG removed')
        assert png_format not in inspect()[0]
        checks.append('registered PNG removed only by image rule')
        drop_effect = RegisterClipboardFormat('Preferred DropEffect')
        configure_rules(dialog, [2])
        write(rich('file   retained') + [(15, hdrop), (drop_effect, struct.pack('<I', 1))])
        assert_clean('file retained')
        with clipboard():
            assert read_locked(drop_effect)[:4] == struct.pack('<I', 1)
            assert read_locked(15)[:len(hdrop)] == hdrop
        checks.append('file paths and drop metadata preserved together')
        configure_rules(dialog, [17])
        write(rich('file removed') + [(15, hdrop), (drop_effect, struct.pack('<I', 1))])
        assert_clean('file removed')
        assert drop_effect not in inspect()[0] and 15 not in inspect()[0]
        checks.append('file paths and drop metadata removed together')
        custom = RegisterClipboardFormat('ClearCopyTestUnknownMixed')
        write(rich('unknown mixed') + [(8, dib), (custom, b'opaque\0')])
        sequence = GetSequence()
        time.sleep(0.15)
        assert HTML in inspect()[0] and GetSequence() == sequence
        checks.append('unsupported mixed format is skipped before mutation')
        virtual_file = RegisterClipboardFormat('FileGroupDescriptorW')
        write(rich('virtual file') + [(virtual_file, b'opaque\0')])
        sequence = GetSequence()
        time.sleep(0.15)
        assert HTML in inspect()[0] and GetSequence() == sequence
        checks.append('virtual file payload is skipped before mutation')

        configure_rules(dialog, [0, 2, 16])
        click_checkbox(dialog, 1005, 0)
        SendMessage(hwnd, 0x0111, 40003, 0)
        process.wait(timeout=3)
        process = subprocess.Popen([executable], env=environment)
        wait_for(lambda: FindWindow(APP_CLASS, None), 'special configuration relaunch')
        hwnd = FindWindow(APP_CLASS, None)
        SendMessage(hwnd, 0x8002, 0, 0)
        dialog = FindWindow('#32770', 'Clear Copy 设置')
        assert IsDlgButtonChecked(dialog, 1005) == 0
        SendMessage(GetDlgItem(dialog, 1007), 0x00F5, 0, 0)
        config = FindWindow('#32770', '特殊清理配置')
        for i in range(18):
            assert IsDlgButtonChecked(config, 1100 + i) == int(i in (0, 2, 16))
        SendMessage(config, 0x0010, 0, 0)
        click_checkbox(dialog, 1005, 1)
        write(rich('  A   B') + [(8, dib)])
        assert_clean('A B')
        assert 8 not in inspect()[0]
        checks.append('master off and individual rules survive restart; re-enable resumes saved combination')

        # Verify migration from the old single mixed-content option.
        SendMessage(hwnd, 0x0111, 40003, 0)
        process.wait(timeout=3)
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, CONFIG) as key:
            winreg.SetValueEx(key, 'CleanMixed', 0, winreg.REG_DWORD, 1)
            winreg.DeleteValue(key, 'SpecialEnabled')
            winreg.DeleteValue(key, 'SpecialRules')
        process = subprocess.Popen([executable], env=environment)
        wait_for(lambda: FindWindow(APP_CLASS, None), 'legacy migration relaunch')
        hwnd = FindWindow(APP_CLASS, None)
        SendMessage(hwnd, 0x8002, 0, 0)
        dialog = FindWindow('#32770', 'Clear Copy 设置')
        assert IsDlgButtonChecked(dialog, 1005) == 1
        SendMessage(GetDlgItem(dialog, 1007), 0x00F5, 0, 0)
        config = FindWindow('#32770', '特殊清理配置')
        for i in range(18):
            assert IsDlgButtonChecked(config, 1100 + i) == int(i in (16, 17))
        SendMessage(config, 0x0010, 0, 0)
        write(rich('legacy mixed') + [(8, dib), (15, hdrop)])
        assert_clean('legacy mixed')
        assert 8 not in inspect()[0] and 15 not in inspect()[0]
        checks.append('legacy CleanMixed migrates to special master plus image/file rules only')

        SendMessage(GetDlgItem(dialog, 1007), 0x00F5, 0, 0)
        assert FindWindow('#32770', '特殊清理配置')
        SendMessage(hwnd, 0x0111, 40003, 0)
        assert process.wait(timeout=3) == 0
        assert not FindWindow(APP_CLASS, None)
        assert not FindWindow('#32770', '特殊清理配置')
        assert not FindWindow('#32770', 'Clear Copy 设置')
        checks.append('exit destroys window and process')
        print('\n'.join('PASS: ' + label for label in checks))
        print(f'{len(checks)} integration checks passed.')
    finally:
        if process and process.poll() is None:
            hwnd = FindWindow(APP_CLASS, None)
            if hwnd:
                SendMessage(hwnd, 0x0111, 40003, 0)
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=3)
        try:
            try:
                winreg.DeleteKey(winreg.HKEY_CURRENT_USER, test_run_key)
            except FileNotFoundError:
                pass
            if config_touched:
                restore_config(saved)
        finally:
            DestroyWindow(OWNER)


if __name__ == '__main__':
    if FindWindow(APP_CLASS, None):
        raise RuntimeError('Exit the existing Clear Copy instance before running integration tests.')
    root = pathlib.Path(__file__).resolve().parents[1]
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-municode', '-mwindows', '-Os',
                    str(root / 'tests' / 'test_host.c'), str(root / 'build' / 'app.res.o'),
                    '-o', str(root / 'build' / 'clear-copy-test.exe'), '-luser32', '-lshell32',
                    '-ladvapi32', '-lcomctl32', '-lgdi32'], check=True)
    with isolated_desktop() as test_environment:
        run_checks(test_environment)
