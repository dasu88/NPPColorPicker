#include "PluginApi.h"
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <windowsx.h>

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Msimg32.lib")

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
static bool g_useNativePickerForAlpha = true;
static wchar_t g_settingsPath[MAX_PATH]{};
static bool g_hasHoverSelection = false;
static HWND g_hoverEditor = nullptr;
static intptr_t g_previousSelectionStart = 0;
static intptr_t g_previousSelectionEnd = 0;
static HWND g_colorClickEditor = nullptr;
static LPARAM g_colorClickPosition = 0;
static bool g_isTraditionalChinese = false;

static void Settings();
static void About();
static FuncItem g_funcs[] = { { L"Color Picker - Settings", Settings, 0, false, nullptr }, { L"Color Picker - About", About, 0, false, nullptr } };

constexpr int ID_SELECT_ON_HOVER = 1001;
constexpr int ID_SAVE_SETTINGS = 1002;
constexpr int ID_USE_NATIVE_PICKER_FOR_ALPHA = 1003;
constexpr int ID_RGBA_RED = 1101;
constexpr int ID_RGBA_GREEN = 1102;
constexpr int ID_RGBA_BLUE = 1103;
constexpr int ID_RGBA_ALPHA = 1104;
constexpr int ID_RGBA_OK = 1105;
constexpr int ID_RGBA_CANCEL = 1106;
constexpr int ID_RGBA_PREVIEW = 1107;
constexpr int ID_RGBA_CODE = 1108;
constexpr int ID_RGBA_RED_LABEL = 1109;
constexpr int ID_RGBA_GREEN_LABEL = 1110;
constexpr int ID_RGBA_BLUE_LABEL = 1111;
constexpr int ID_RGBA_ALPHA_LABEL = 1112;

struct ColorToken { intptr_t start = -1; intptr_t end = -1; COLORREF color{}; bool shortForm = false; };

static bool IsHex(char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F'); }
static int Hex(char value) { return value <= '9' ? value - '0' : (value <= 'F' ? value - 'A' + 10 : value - 'a' + 10); }
static char At(HWND editor, intptr_t position) { return static_cast<char>(SendMessage(editor, SCI_GETCHARAT, position, 0)); }

static void RefreshUiLanguage() {
    char languageFile[MAX_PATH]{};
    SendMessage(g_npp._nppHandle, NPPM_GETNATIVELANGFILENAME, sizeof(languageFile), reinterpret_cast<LPARAM>(languageFile));
    g_isTraditionalChinese = _stricmp(languageFile, "chinese.xml") == 0 || _stricmp(languageFile, "taiwaneseMandarin.xml") == 0 || _stricmp(languageFile, "hongKongCantonese.xml") == 0;
    wcscpy_s(g_funcs[0]._itemName, g_isTraditionalChinese ? L"色彩選擇器 - 設定" : L"Color Picker - Settings");
    wcscpy_s(g_funcs[1]._itemName, g_isTraditionalChinese ? L"色彩選擇器 - 關於" : L"Color Picker - About");
}

struct RgbaPickerData { int red; int green; int blue; int alpha; bool accepted = false; HWND window = nullptr; };

static void UpdateRgbaPreview(HWND window, RgbaPickerData* data) {
    data->red = static_cast<int>(GetWindowLongPtr(GetDlgItem(window, ID_RGBA_RED), GWLP_USERDATA));
    data->green = static_cast<int>(GetWindowLongPtr(GetDlgItem(window, ID_RGBA_GREEN), GWLP_USERDATA));
    data->blue = static_cast<int>(GetWindowLongPtr(GetDlgItem(window, ID_RGBA_BLUE), GWLP_USERDATA));
    data->alpha = static_cast<int>(GetWindowLongPtr(GetDlgItem(window, ID_RGBA_ALPHA), GWLP_USERDATA));
    wchar_t code[10]{};
    swprintf_s(code, L"#%02X%02X%02X%02X", data->red, data->green, data->blue, data->alpha);
    SetDlgItemText(window, ID_RGBA_CODE, code);
    InvalidateRect(window, nullptr, TRUE);
}

static int BlendChannel(int foreground, int background, int alpha) { return (foreground * alpha + background * (255 - alpha) + 127) / 255; }

static void SetCustomSliderValue(HWND slider, int x) {
    RECT rect{}; GetClientRect(slider, &rect);
    constexpr int margin = 8;
    const int width = rect.right - rect.left - margin * 2;
    int value = (x - margin) * 255 / width;
    if (value < 0) value = 0; else if (value > 255) value = 255;
    SetWindowLongPtr(slider, GWLP_USERDATA, value);
    InvalidateRect(slider, nullptr, TRUE);
    SendMessage(GetParent(slider), WM_HSCROLL, 0, reinterpret_cast<LPARAM>(slider));
}

static LRESULT CALLBACK RgbaSliderProc(HWND slider, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(slider, &paint); RECT rect{}; GetClientRect(slider, &rect);
        FillRect(dc, &rect, GetSysColorBrush(COLOR_BTNFACE));
        constexpr int margin = 8, railHeight = 4, thumbWidth = 10;
        const int centerY = (rect.bottom - rect.top) / 2;
        const RECT rail{ margin, centerY - railHeight / 2, rect.right - margin, centerY + railHeight / 2 };
        FillRect(dc, &rail, GetSysColorBrush(COLOR_3DSHADOW));
        const int value = static_cast<int>(GetWindowLongPtr(slider, GWLP_USERDATA));
        const int thumbCenter = margin + value * (rect.right - rect.left - margin * 2) / 255;
        const RECT thumb{ thumbCenter - thumbWidth / 2, centerY - 9, thumbCenter + thumbWidth / 2, centerY + 9 };
        FillRect(dc, &thumb, GetSysColorBrush(COLOR_HIGHLIGHT));
        FrameRect(dc, &thumb, GetSysColorBrush(COLOR_3DSHADOW));
        EndPaint(slider, &paint); return 0;
    }
    if (message == WM_LBUTTONDOWN) { SetCapture(slider); SetCustomSliderValue(slider, GET_X_LPARAM(lParam)); return 0; }
    if (message == WM_MOUSEMOVE && GetCapture() == slider) { SetCustomSliderValue(slider, GET_X_LPARAM(lParam)); return 0; }
    if (message == WM_LBUTTONUP && GetCapture() == slider) { SetCustomSliderValue(slider, GET_X_LPARAM(lParam)); ReleaseCapture(); return 0; }
    if (message == WM_SETCURSOR) { SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); return TRUE; }
    return DefWindowProc(slider, message, wParam, lParam);
}

static LRESULT CALLBACK RgbaPickerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<RgbaPickerData*>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_CREATE) {
        data = reinterpret_cast<RgbaPickerData*>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
        const HINSTANCE instance = GetModuleHandle(nullptr);
        struct Channel { int labelId; int id; int value; };
        const Channel channels[] = { { ID_RGBA_RED_LABEL, ID_RGBA_RED, data->red }, { ID_RGBA_GREEN_LABEL, ID_RGBA_GREEN, data->green }, { ID_RGBA_BLUE_LABEL, ID_RGBA_BLUE, data->blue }, { ID_RGBA_ALPHA_LABEL, ID_RGBA_ALPHA, data->alpha } };
        for (int index = 0; index < 4; ++index) {
            const int y = 18 + index * 32;
            HWND slider = CreateWindow(L"NppColorPickerRgbaSlider", L"", WS_CHILD | WS_VISIBLE,
                90, y, 138, 25, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(channels[index].id)), instance, nullptr);
            SetWindowLongPtr(slider, GWLP_USERDATA, channels[index].value);
        }
        CreateWindow(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            238, 18, 62, 116, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RGBA_PREVIEW)), instance, nullptr);
        CreateWindow(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
            20, 151, 280, 20, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RGBA_CODE)), instance, nullptr);
        CreateWindow(L"STATIC", g_isTraditionalChinese ? L"透明: 0          不透明: 255" : L"Alpha: 0 = transparent, 255 = opaque.", WS_CHILD | WS_VISIBLE | SS_CENTER,
            20, 174, 280, 20, window, nullptr, instance, nullptr);
        CreateWindow(L"BUTTON", g_isTraditionalChinese ? L"確定" : L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            78, 204, 80, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RGBA_OK)), instance, nullptr);
        CreateWindow(L"BUTTON", g_isTraditionalChinese ? L"取消" : L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            168, 204, 80, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RGBA_CANCEL)), instance, nullptr);
        UpdateRgbaPreview(window, data);
        return 0;
    }
    if (message == WM_PAINT && data) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        SetBkMode(dc, TRANSPARENT);
        const int values[] = { data->red, data->green, data->blue, data->alpha };
        const wchar_t* formats[] = {
            g_isTraditionalChinese ? L"紅: %d" : L"Red: %d", g_isTraditionalChinese ? L"綠: %d" : L"Green: %d",
            g_isTraditionalChinese ? L"藍: %d" : L"Blue: %d", g_isTraditionalChinese ? L"透明度: %d" : L"Alpha: %d"
        };
        for (int index = 0; index < 4; ++index) {
            wchar_t label[16]{}; swprintf_s(label, formats[index], values[index]);
            RECT labelRect{ 20, 18 + index * 32, 88, 43 + index * 32 };
            DrawText(dc, label, -1, &labelRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        }
        EndPaint(window, &paint); return 0;
    }
    if (message == WM_DRAWITEM && data && wParam == ID_RGBA_PREVIEW) {
        const DRAWITEMSTRUCT* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        const int square = 8;
        for (int y = draw->rcItem.top; y < draw->rcItem.bottom; y += square) {
            for (int x = draw->rcItem.left; x < draw->rcItem.right; x += square) {
                const int background = ((x - draw->rcItem.left) / square + (y - draw->rcItem.top) / square) % 2 ? 205 : 245;
                const COLORREF color = RGB(BlendChannel(data->red, background, data->alpha), BlendChannel(data->green, background, data->alpha), BlendChannel(data->blue, background, data->alpha));
                const RECT squareRect{ x, y, x + square < draw->rcItem.right ? x + square : draw->rcItem.right, y + square < draw->rcItem.bottom ? y + square : draw->rcItem.bottom };
                HBRUSH brush = CreateSolidBrush(color); FillRect(draw->hDC, &squareRect, brush); DeleteObject(brush);
            }
        }
        FrameRect(draw->hDC, &draw->rcItem, GetSysColorBrush(COLOR_WINDOWFRAME));
        return TRUE;
    }
    if (message == WM_COMMAND && data) {
        if (LOWORD(wParam) == ID_RGBA_OK) {
            UpdateRgbaPreview(window, data);
            data->accepted = true; DestroyWindow(window); return 0;
        }
        if (LOWORD(wParam) == ID_RGBA_CANCEL) { DestroyWindow(window); return 0; }
    }
    if (message == WM_HSCROLL && data) {
        const HWND slider = reinterpret_cast<HWND>(lParam);
        const int id = GetDlgCtrlID(slider);
        if (id >= ID_RGBA_RED && id <= ID_RGBA_ALPHA) { UpdateRgbaPreview(window, data); return 0; }
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY && data) { data->window = nullptr; return 0; }
    return DefWindowProc(window, message, wParam, lParam);
}

static bool ChooseRgba(COLORREF color, int& red, int& green, int& blue, int& alpha) {
    RefreshUiLanguage();
    RgbaPickerData data{ GetRValue(color), GetGValue(color), GetBValue(color), alpha };
    WNDCLASS sliderClass{};
    sliderClass.hInstance = GetModuleHandle(nullptr); sliderClass.lpszClassName = L"NppColorPickerRgbaSlider";
    sliderClass.lpfnWndProc = RgbaSliderProc; sliderClass.hCursor = LoadCursor(nullptr, IDC_SIZEWE);
    RegisterClass(&sliderClass);
    WNDCLASS windowClass{};
    windowClass.hInstance = GetModuleHandle(nullptr); windowClass.lpszClassName = L"NppColorPickerRgba";
    windowClass.lpfnWndProc = RgbaPickerProc; windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW); windowClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    RegisterClass(&windowClass);
    constexpr int width = 320, height = 275;
    RECT ownerRect{}; GetWindowRect(g_npp._nppHandle, &ownerRect);
    data.window = CreateWindowEx(WS_EX_DLGMODALFRAME, windowClass.lpszClassName, g_isTraditionalChinese ? L"RGBA 色彩選擇器" : L"RGBA Color Picker",
        WS_CAPTION | WS_SYSMENU | WS_POPUP, ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2,
        ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2, width, height, g_npp._nppHandle, nullptr, windowClass.hInstance, &data);
    if (!data.window) return false;
    EnableWindow(g_npp._nppHandle, FALSE);
    ShowWindow(data.window, SW_SHOW); SetForegroundWindow(data.window); SetActiveWindow(data.window);
    MSG message{};
    while (data.window && GetMessage(&message, nullptr, 0, 0)) {
        if (!IsDialogMessage(data.window, &message)) { TranslateMessage(&message); DispatchMessage(&message); }
    }
    EnableWindow(g_npp._nppHandle, TRUE); SetForegroundWindow(g_npp._nppHandle);
    if (!data.accepted) return false;
    red = data.red; green = data.green; blue = data.blue; alpha = data.alpha;
    return true;
}

static ColorToken TokenAt(HWND editor, intptr_t position) {
    if (position < 0) return {};
    intptr_t hash = position;
    while (hash >= 0 && position - hash <= 8 && At(editor, hash) != '#') --hash;
    if (hash < 0 || At(editor, hash) != '#') return {};
    if (hash > 0 && IsHex(At(editor, hash - 1))) return {};
    int digits = 0;
    while (digits < 9 && IsHex(At(editor, hash + 1 + digits))) ++digits;
    if (digits != 3 && digits != 6 && digits != 8) return {};
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

static UINT_PTR CALLBACK ChooseColorHook(HWND dialog, UINT message, WPARAM, LPARAM) {
    if (message != WM_INITDIALOG) return FALSE;
    RECT dialogRect{}; GetWindowRect(dialog, &dialogRect);
    HMONITOR monitor = MonitorFromWindow(g_npp._nppHandle, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{ sizeof(monitorInfo) }; GetMonitorInfo(monitor, &monitorInfo);
    const RECT& workArea = monitorInfo.rcWork;
    const int width = dialogRect.right - dialogRect.left, height = dialogRect.bottom - dialogRect.top;
    const int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
    const int y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;
    SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return FALSE;
}

static bool PickColor(HWND editor, LPARAM mousePosition) {
    const ColorToken token = TokenAt(editor, SendMessage(editor, SCI_POSITIONFROMPOINTCLOSE, GET_X_LPARAM(mousePosition), GET_Y_LPARAM(mousePosition)));
    if (token.start < 0) return false;
    RestoreHoverSelection(editor);
    const bool hasAlpha = token.end - token.start == 9;
    int red = GetRValue(token.color), green = GetGValue(token.color), blue = GetBValue(token.color);
    int alpha = hasAlpha ? Hex(At(editor, token.end - 2)) * 16 + Hex(At(editor, token.end - 1)) : 255;
    const bool useCustomPicker = hasAlpha && (!g_useNativePickerForAlpha || (GetKeyState(VK_SHIFT) & 0x8000));
    if (useCustomPicker) {
        if (!ChooseRgba(token.color, red, green, blue, alpha)) return true;
    } else {
        CHOOSECOLOR chooser{};
        chooser.lStructSize = sizeof(chooser); chooser.hwndOwner = g_npp._nppHandle; chooser.rgbResult = token.color;
        chooser.lpCustColors = g_customColors; chooser.lpfnHook = ChooseColorHook; chooser.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ENABLEHOOK;
        if (!ChooseColor(&chooser)) return true;
        red = GetRValue(chooser.rgbResult); green = GetGValue(chooser.rgbResult); blue = GetBValue(chooser.rgbResult);
    }
    char replacement[10]{};
    if (token.shortForm && red % 17 == 0 && green % 17 == 0 && blue % 17 == 0) sprintf_s(replacement, "#%1X%1X%1X", red / 17, green / 17, blue / 17);
    else sprintf_s(replacement, "#%02X%02X%02X", red, green, blue);
    if (hasAlpha) {
        const size_t length = strlen(replacement);
        sprintf_s(replacement + length, sizeof(replacement) - length, "%02X", alpha);
    }
    // Set the target only after the modal dialog closes. This replaces the original token without changing Notepad++'s visible selection.
    SendMessage(editor, SCI_SETTARGETSTART, token.start, 0); SendMessage(editor, SCI_SETTARGETEND, token.end, 0);
    SendMessage(editor, SCI_REPLACETARGET, strlen(replacement), reinterpret_cast<LPARAM>(replacement));
    return true;
}

static LRESULT CALLBACK ScintillaProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_MOUSEMOVE) { TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&tracking); ShowPreview(hwnd, lParam); }
    if (message == WM_MOUSELEAVE) { if (g_preview) ShowWindow(g_preview, SW_HIDE); RestoreHoverSelection(hwnd); }
    if (message == WM_LBUTTONDOWN) {
        const intptr_t position = SendMessage(hwnd, SCI_POSITIONFROMPOINTCLOSE, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (TokenAt(hwnd, position).start >= 0) { g_colorClickEditor = hwnd; g_colorClickPosition = lParam; SetCapture(hwnd); return 0; }
    }
    if (message == WM_LBUTTONUP && g_colorClickEditor == hwnd) {
        const LPARAM colorPosition = g_colorClickPosition;
        g_colorClickEditor = nullptr; g_colorClickPosition = 0;
        if (GetCapture() == hwnd) ReleaseCapture();
        PickColor(hwnd, colorPosition); return 0;
    }
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
    g_useNativePickerForAlpha = GetPrivateProfileInt(L"Settings", L"UseNativePickerForAlpha", 1, g_settingsPath) != 0;
}

static void SaveSettings() {
    WritePrivateProfileString(L"Settings", L"SelectOnHover", g_selectOnHover ? L"1" : L"0", g_settingsPath);
    WritePrivateProfileString(L"Settings", L"UseNativePickerForAlpha", g_useNativePickerForAlpha ? L"1" : L"0", g_settingsPath);
}

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        CreateWindow(L"BUTTON", g_isTraditionalChinese ? L"滑鼠停留時選取色碼文字" : L"Select color code text on hover", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            18, 18, 240, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SELECT_ON_HOVER)), GetModuleHandle(nullptr), nullptr);
        SendDlgItemMessage(hwnd, ID_SELECT_ON_HOVER, BM_SETCHECK, g_selectOnHover ? BST_CHECKED : BST_UNCHECKED, 0);
        CreateWindow(L"BUTTON", g_isTraditionalChinese ? L"針對 #RRGGBBAA 使用原生選色器" : L"Use native color picker for #RRGGBBAA", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            18, 48, 260, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_USE_NATIVE_PICKER_FOR_ALPHA)), GetModuleHandle(nullptr), nullptr);
        SendDlgItemMessage(hwnd, ID_USE_NATIVE_PICKER_FOR_ALPHA, BM_SETCHECK, g_useNativePickerForAlpha ? BST_CHECKED : BST_UNCHECKED, 0);
        CreateWindow(L"BUTTON", g_isTraditionalChinese ? L"確定" : L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            105, 88, 80, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SAVE_SETTINGS)), GetModuleHandle(nullptr), nullptr);
        return 0;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == ID_SAVE_SETTINGS) {
        g_selectOnHover = SendDlgItemMessage(hwnd, ID_SELECT_ON_HOVER, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_useNativePickerForAlpha = SendDlgItemMessage(hwnd, ID_USE_NATIVE_PICKER_FOR_ALPHA, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SaveSettings(); DestroyWindow(hwnd); return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (message == WM_DESTROY) { g_settingsWindow = nullptr; return 0; }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

static void Settings() {
    RefreshUiLanguage();
    if (g_settingsWindow) { SetForegroundWindow(g_settingsWindow); return; }
    WNDCLASS windowClass{};
    windowClass.hInstance = GetModuleHandle(nullptr); windowClass.lpszClassName = L"NppColorPickerSettings";
    windowClass.lpfnWndProc = SettingsProc; windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&windowClass);
    constexpr int windowWidth = 280;
    constexpr int windowHeight = 165;
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
    g_settingsWindow = CreateWindowEx(WS_EX_DLGMODALFRAME, windowClass.lpszClassName, g_isTraditionalChinese ? L"色彩選擇器設定" : L"Color Picker Settings",
        WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE, x, y, windowWidth, windowHeight,
        g_npp._nppHandle, nullptr, windowClass.hInstance, nullptr);
}

static void About() { RefreshUiLanguage(); MessageBox(g_npp._nppHandle, g_isTraditionalChinese ? L"將滑鼠停留於 #RGB、#RRGGBB 或 #RRGGBBAA 上即可預覽色彩。點擊色碼可進行取代。\r\n\r\n針對 #RRGGBBAA，可在設定中選擇使用原生 RGB 選色器或自製 RGBA 選色器。\r\n\r\n作者：dasu88\r\n電子郵件：dasu88@gmail.com" : L"Hover #RGB, #RRGGBB, or #RRGGBBAA to preview its color. Click a color code to replace it.\r\n\r\nFor #RRGGBBAA, choose the native RGB picker or the custom RGBA picker in Settings.\r\n\r\nAuthor: dasu88\r\nEmail: dasu88@gmail.com", g_isTraditionalChinese ? L"Notepad++ 色彩選擇器" : L"Notepad++ Color Picker", MB_OK | MB_ICONINFORMATION); }

extern "C" __declspec(dllexport) void setInfo(NppData data) { g_npp = data; RefreshUiLanguage(); LoadSettings(); }
extern "C" __declspec(dllexport) const wchar_t* getName() { return L"ColorPicker"; }
extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count) { *count = 2; return g_funcs; }
extern "C" __declspec(dllexport) void beNotified(SCNotification* notification) { if (!notification) return; if (notification->nmhdr.code == NPPN_READY) { RefreshUiLanguage(); Attach(); } if (notification->nmhdr.code == NPPN_NATIVELANGCHANGED) RefreshUiLanguage(); }
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }
extern "C" __declspec(dllexport) BOOL isUnicode() { return TRUE; }
