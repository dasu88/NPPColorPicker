#pragma once
#include <windows.h>

// Notepad++ plugin API constants required by this plugin.
#define NPPMSG  (WM_USER + 1000)
#define NPPM_GETCURRENTSCINTILLA (NPPMSG + 4)
#define NPPM_GETSCINTILLAHANDLE  (NPPMSG + 5)
#define NPPN_FIRST 1000
#define NPPN_READY (NPPN_FIRST + 1)

struct SCNotification {
    NMHDR nmhdr; intptr_t position; int ch; int modifiers; int modificationType;
    const char* text; intptr_t length; intptr_t linesAdded; int message;
    uintptr_t wParam; intptr_t lParam; intptr_t line; int foldLevelNow;
    int foldLevelPrev; int margin; int listType; int x; int y;
};

struct NppData { HWND _nppHandle; HWND _scintillaMainHandle; HWND _scintillaSecondHandle; };
struct ShortcutKey { bool _isCtrl; bool _isAlt; bool _isShift; unsigned char _key; };
struct FuncItem { wchar_t _itemName[64]; void (*_pFunc)(); int _cmdID; bool _init2Check; ShortcutKey* _pShKey; };
