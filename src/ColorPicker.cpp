#include "PluginApi.h"
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <windowsx.h>

#pragma comment(lib, "Comdlg32.lib")

constexpr UINT SCI_GETCHARAT = 2007;
constexpr UINT SCI_SETTARGETSTART = 2190;
constexpr UINT SCI_SETTARGETEND = 2192;
constexpr UINT SCI_REPLACETARGET = 2194;
constexpr UINT SCI_POSITIONFROMPOINTCLOSE = 2023;
constexpr UINT SCI_SETSEL = 2160;
constexpr UINT SCI_GETSELECTIONSTART = 2143;
constexpr UINT SCI_GETSELECTIONEND = 2145;
constexpr UINT SCI_GETFIRSTVISIBLELINE = 2152;
constexpr UINT SCI_SETFIRSTVISIBLELINE = 2613;

static NppData g_npp{};
static HWND g_preview = nullptr;
static WNDPROC g_oldMainProc = nullptr;
static WNDPROC g_oldSecondProc = nullptr;
static COLORREF g_previewColor{};
static COLORREF g_customColors[16]{};
static HWND g_settingsWindow = nullptr;
static bool g_selectOnHover = true;
static wchar_t g_settingsPath[MAX_PATH]{};
static bool g_hasHoverSelection = false;
static HWND g_hoverEditor = nullptr;
static intptr_t g_previousSelectionStart = 0;
static intptr_t g_previousSelectionEnd = 0;

constexpr int ID_SELECT_ON_HOVER = 1001;
constexpr int ID_SAVE_SETTINGS = 1002;

struct ColorToken { intptr_t start = -1; intptr_t end = -1; COLORREF color{}; bool shortForm = false; };

static bool IsHex(char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F'); }
static int Hex(char value) { return value <= '9' ? value - '0' : (value <= 'F' ? value - 'A' + 10 : value - 'a' + 10); }
static char At(HWND editor, intptr_t position) { return static_cast<char>(SendMessage(editor, SCI_GETCHARAT, position, 0)); }

static ColorToken TokenAt(HWND editor, intptr_t position) {
    if (position < 0) return {};
    intptr_t hash = position;
    while (hash >= 0 && position - hash <= 6 && At(editor, hash) != '#') --hash;
    if (hash < 0 || At(editor, hash) != '#') return {};
    if (hash > 0 && IsHex(At(editor, hash - 1))) return {};
    int digits = 0;
    while (digits < 7 && IsHex(At(editor, hash + 1 + digits))) ++digits;
    if (digits != 3 && digits != 6) return {};
    if (IsHex(At(editor, hash + 1 + digits)) || position < hash || position > hash + digits) return {};
    int red = Hex(At(editor, hash + 1));
    int green = Hex(At(editor, hash + 2));
    int blue = Hex(At(editor, hash + 3));
    if (digits == 3) { red *= 17; green *= 17; blue *= 17; }
    else { red = red * 16 + Hex(At(editor, hash + 2)); green = Hex(At(editor, hash + 3)) * 16 + Hex(At(editor, hash + 4)); blue = Hex(At(editor, hash + 5)) * 16 + Hex(At(editor, hash + 6)); }
    return { hash, hash + digits + 1, RGB(red, green, blue), digits == 3 };
}

static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint); RECT rect{}; GetClientRect(hwnd, &rect);
        HBRUSH brush = CreateSolidBrush(g_previewColor); FillRect(dc, &rect, brush); DeleteObject(brush);
        FrameRect(dc, &rect, GetSysColorBrush(COLOR_WINDOWFRAME)); EndPaint(hwnd, &paint); return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

static void RestoreHoverSelection(HWND editor) {
    if (!g_hasHoverSelection || editor != g_hoverEditor) return;
    const intptr_t firstVisibleLine = SendMessage(editor, SCI_GETFIRSTVISIBLELINE, 0, 0);
    SendMessage(editor, SCI_SETSEL, g_previousSelectionStart, g_previousSelectionEnd);
    SendMessage(editor, SCI_SETFIRSTVISIBLELINE, firstVisibleLine, 0);
    g_hasHoverSelection = false;
    g_hoverEditor = nullptr;
}

static void SelectHoverToken(HWND editor, const ColorToken& token) {
    if (!g_selectOnHover) return;
    if (!g_hasHoverSelection) {
        g_previousSelectionStart = SendMessage(editor, SCI_GETSELECTIONSTART, 0, 0);
        g_previousSelectionEnd = SendMessage(editor, SCI_GETSELECTIONEND, 0, 0);
        g_hoverEditor = editor;
        g_hasHoverSelection = true;
    }
    const intptr_t firstVisibleLine = SendMessage(editor, SCI_GETFIRSTVISIBLELINE, 0, 0);
    SendMessage(editor, SCI_SETSEL, token.start, token.end);
    SendMessage(editor, SCI_SETFIRSTVISIBLELINE, firstVisibleLine, 0);
}

static void EnsurePreview() {
    if (g_preview) return;
    WNDCLASS windowClass{};
    windowClass.hInstance = GetModuleHandle(nullptr); windowClass.lpszClassName = L"NppColorPickerPreview";
    windowClass.lpfnWndProc = PreviewProc; windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&windowClass);
    g_preview = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, windowClass.lpszClassName,
        L"", WS_POPUP | WS_BORDER, 0, 0, 30, 30, g_npp._nppHandle, nullptr, windowClass.hInstance, nullptr);
}

static void ShowPreview(HWND editor, LPARAM mousePosition) {
    POINT point{ GET_X_LPARAM(mousePosition), GET_Y_LPARAM(mousePosition) };
    const ColorToken token = TokenAt(editor, SendMessage(editor, SCI_POSITIONFROMPOINTCLOSE, point.x, point.y));
    if (token.start < 0) { if (g_preview) ShowWindow(g_preview, SW_HIDE); RestoreHoverSelection(editor); return; }
    SelectHoverToken(editor, token);
    EnsurePreview(); g_previewColor = token.color; ClientToScreen(editor, &point);
    SetWindowPos(g_preview, HWND_TOPMOST, point.x + 14, point.y + 18, 30, 30, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_preview, nullptr, TRUE);
}

static void PickColor(HWND editor, LPARAM mousePosition) {
    const ColorToken token = TokenAt(editor, SendMessage(editor, SCI_POSITIONFROMPOINTCLOSE, GET_X_LPARAM(mousePosition), GET_Y_LPARAM(mousePosition)));
    if (token.start < 0) return;
    CHOOSECOLOR chooser{};
    chooser.lStructSize = sizeof(chooser); chooser.hwndOwner = g_npp._nppHandle; chooser.rgbResult = token.color;
    chooser.lpCustColors = g_customColors; chooser.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColor(&chooser)) return;
    const int red = GetRValue(chooser.rgbResult), green = GetGValue(chooser.rgbResult), blue = GetBValue(chooser.rgbResult);
    char replacement[8]{};
    if (token.shortForm && red % 17 == 0 && green % 17 == 0 && blue % 17 == 0) sprintf_s(replacement, "#%1X%1X%1X", red / 17, green / 17, blue / 17);
    else sprintf_s(replacement, "#%02X%02X%02X", red, green, blue);
    SendMessage(editor, SCI_SETTARGETSTART, token.start, 0); SendMessage(editor, SCI_SETTARGETEND, token.end, 0);
    SendMessage(editor, SCI_REPLACETARGET, strlen(replacement), reinterpret_cast<LPARAM>(replacement));
}

static LRESULT CALLBACK ScintillaProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_MOUSEMOVE) { TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&tracking); ShowPreview(hwnd, lParam); }
    if (message == WM_MOUSELEAVE) { if (g_preview) ShowWindow(g_preview, SW_HIDE); RestoreHoverSelection(hwnd); }
    if (message == WM_LBUTTONUP) PickColor(hwnd, lParam);
    const WNDPROC oldProc = hwnd == g_npp._scintillaMainHandle ? g_oldMainProc : g_oldSecondProc;
    return CallWindowProc(oldProc, hwnd, message, wParam, lParam);
}

static void Attach() {
    if (g_npp._scintillaMainHandle && !g_oldMainProc) g_oldMainProc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(g_npp._scintillaMainHandle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ScintillaProc)));
    if (g_npp._scintillaSecondHandle && !g_oldSecondProc) g_oldSecondProc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(g_npp._scintillaSecondHandle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ScintillaProc)));
}

static void LoadSettings() {
    wchar_t appData[MAX_PATH]{};
    if (GetEnvironmentVariable(L"APPDATA", appData, MAX_PATH) == 0) return;
    swprintf_s(g_settingsPath, L"%s\\Notepad++\\plugins\\config\\ColorPicker.ini", appData);
    wchar_t directory[MAX_PATH]{};
    swprintf_s(directory, L"%s\\Notepad++", appData); CreateDirectory(directory, nullptr);
    swprintf_s(directory, L"%s\\Notepad++\\plugins", appData); CreateDirectory(directory, nullptr);
    swprintf_s(directory, L"%s\\Notepad++\\plugins\\config", appData); CreateDirectory(directory, nullptr);
    g_selectOnHover = GetPrivateProfileInt(L"Settings", L"SelectOnHover", 1, g_settingsPath) != 0;
}

static void SaveSettings() { WritePrivateProfileString(L"Settings", L"SelectOnHover", g_selectOnHover ? L"1" : L"0", g_settingsPath); }

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        CreateWindow(L"BUTTON", L"Select color code text on hover", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            18, 18, 240, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SELECT_ON_HOVER)), GetModuleHandle(nullptr), nullptr);
        SendDlgItemMessage(hwnd, ID_SELECT_ON_HOVER, BM_SETCHECK, g_selectOnHover ? BST_CHECKED : BST_UNCHECKED, 0);
        CreateWindow(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            105, 56, 80, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SAVE_SETTINGS)), GetModuleHandle(nullptr), nullptr);
        return 0;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == ID_SAVE_SETTINGS) {
        g_selectOnHover = SendDlgItemMessage(hwnd, ID_SELECT_ON_HOVER, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SaveSettings(); DestroyWindow(hwnd); return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (message == WM_DESTROY) { g_settingsWindow = nullptr; return 0; }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

static void Settings() {
    if (g_settingsWindow) { SetForegroundWindow(g_settingsWindow); return; }
    WNDCLASS windowClass{};
    windowClass.hInstance = GetModuleHandle(nullptr); windowClass.lpszClassName = L"NppColorPickerSettings";
    windowClass.lpfnWndProc = SettingsProc; windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&windowClass);
    constexpr int windowWidth = 280;
    constexpr int windowHeight = 130;
    RECT ownerRect{};
    GetWindowRect(g_npp._nppHandle, &ownerRect);
    HMONITOR monitor = MonitorFromWindow(g_npp._nppHandle, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    GetMonitorInfo(monitor, &monitorInfo);
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - windowWidth) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - windowHeight) / 2;
    if (x < monitorInfo.rcWork.left) x = monitorInfo.rcWork.left;
    if (x > monitorInfo.rcWork.right - windowWidth) x = monitorInfo.rcWork.right - windowWidth;
    if (y < monitorInfo.rcWork.top) y = monitorInfo.rcWork.top;
    if (y > monitorInfo.rcWork.bottom - windowHeight) y = monitorInfo.rcWork.bottom - windowHeight;
    g_settingsWindow = CreateWindowEx(WS_EX_DLGMODALFRAME, windowClass.lpszClassName, L"Color Picker Settings",
        WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE, x, y, windowWidth, windowHeight,
        g_npp._nppHandle, nullptr, windowClass.hInstance, nullptr);
}

static void About() { MessageBox(g_npp._nppHandle, L"Hover #RGB or #RRGGBB to preview its color. Click a color code to replace it.\r\n\r\nAuthor: dasu88\r\nEmail: dasu88@gmail.com", L"Notepad++ Color Picker", MB_OK | MB_ICONINFORMATION); }
static FuncItem g_funcs[] = { { L"Color Picker - Settings", Settings, 0, false, nullptr }, { L"Color Picker - About", About, 0, false, nullptr } };

extern "C" __declspec(dllexport) void setInfo(NppData data) { g_npp = data; LoadSettings(); }
extern "C" __declspec(dllexport) const wchar_t* getName() { return L"ColorPicker"; }
extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count) { *count = 2; return g_funcs; }
extern "C" __declspec(dllexport) void beNotified(SCNotification* notification) { if (notification && notification->nmhdr.code == NPPN_READY) Attach(); }
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }
extern "C" __declspec(dllexport) BOOL isUnicode() { return TRUE; }
