// retropad - a Petzold-style Win32 notepad clone implemented in mostly plain C.
// Keeps the classic menus/accelerators, word wrap, status bar, find/replace,
// font picker, and basic file load/save with BOM detection.
#include "file_io.h"
#include "resource.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <strsafe.h>
#include <windows.h>

#define APP_TITLE L"retropad"
#define UNTITLED_NAME L"Untitled"
#define MAX_PATH_BUFFER 1024
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480

#define MAX_TABS 32
#ifndef EM_SETZOOM
#define EM_SETZOOM (WM_USER + 225)
#endif
#ifndef EM_GETZOOM
#define EM_GETZOOM (WM_USER + 226)
#endif

typedef struct TabData {
  HWND hwndEdit;
  WCHAR currentPath[MAX_PATH_BUFFER];
  BOOL modified;
  TextEncoding encoding;
  LineEnding lineEnding;
} TabData;

typedef struct AppState {
  HWND hwndMain;
  HWND hwndTab;
  HWND hwndStatus;
  HFONT hFont;
  LOGFONTW baseFont;
  BOOL wordWrap;
  BOOL statusVisible;
  BOOL statusBeforeWrap;
  FINDREPLACEW find;
  HWND hFindDlg;
  HWND hReplaceDlg;
  UINT findFlags;
  WCHAR findText[128];
  WCHAR replaceText[128];

  TabData tabs[MAX_TABS];
  int tabCount;
  int currentTabIdx;

  HWND hwndLineNumbers;
  BOOL lineNumbersVisible;
  int zoomLevel;
} AppState;

static AppState g_app = {0};
static HINSTANCE g_hInst = NULL;
static UINT g_findMsg = 0;

static void UpdateTitle(HWND hwnd);
static void CreateTabControl(HWND hwnd);
static void AddTab(HWND hwnd, LPCWSTR title, LPCWSTR path);
static BOOL CloseTab(HWND hwnd, int index);
static void ShowTabContextMenu(HWND hwnd, int index);
static void SwitchToTab(HWND hwnd, int index);
static void CreateEditControlForTab(HWND hwnd, int index);
static void UpdateLayout(HWND hwnd);
static BOOL PromptSaveChanges(HWND hwnd, int index);
static BOOL DoFileOpen(HWND hwnd);
static BOOL DoFileSave(HWND hwnd, int index, BOOL saveAs);
static void DoFileNew(HWND hwnd);
static void SetWordWrap(HWND hwnd, BOOL enabled);
static void ToggleStatusBar(HWND hwnd, BOOL visible);
static void UpdateStatusBar(HWND hwnd);
static void ToggleLineNumbers(HWND hwnd, BOOL visible);
static void ShowFindDialog(HWND hwnd);
static void ShowReplaceDialog(HWND hwnd);
static void ShowTabSelector(HWND hwnd);
static void ShowLineEndingSelector(HWND hwnd);
static void ShowEncodingSelector(HWND hwnd);
static void SetZoom(HWND hwnd, int level);
static BOOL DoFindNext(BOOL reverse);
static void DoSelectFont(HWND hwnd);
static void InsertTimeDate(HWND hwnd);
static void HandleFindReplace(LPFINDREPLACEW lpfr);
static BOOL LoadDocumentFromPath(HWND hwnd, int index, LPCWSTR path);
static INT_PTR CALLBACK GoToDlgProc(HWND dlg, UINT msg, WPARAM wParam,
                                    LPARAM lParam);
static INT_PTR CALLBACK AboutDlgProc(HWND dlg, UINT msg, WPARAM wParam,
                                     LPARAM lParam);

static TabData *GetCurrentTab() {
  if (g_app.currentTabIdx >= 0 && g_app.currentTabIdx < g_app.tabCount) {
    return &g_app.tabs[g_app.currentTabIdx];
  }
  return NULL;
}

static HWND GetCurrentEdit() {
  TabData *tab = GetCurrentTab();
  return tab ? tab->hwndEdit : NULL;
}

static BOOL GetEditText(HWND hwndEdit, WCHAR **bufferOut, int *lengthOut) {
  int length = GetWindowTextLengthW(hwndEdit);
  WCHAR *buffer =
      (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
  if (!buffer)
    return FALSE;
  GetWindowTextW(hwndEdit, buffer, length + 1);
  if (lengthOut)
    *lengthOut = length;
  *bufferOut = buffer;
  return TRUE;
}

static BOOL FindInEdit(HWND hwndEdit, const WCHAR *needle, BOOL matchCase,
                       BOOL searchDown, DWORD startPos, DWORD *outStart,
                       DWORD *outEnd) {
  if (!needle || needle[0] == L'\0')
    return FALSE;

  WCHAR *text = NULL;
  int len = 0;
  if (!GetEditText(hwndEdit, &text, &len))
    return FALSE;

  size_t needleLen = wcslen(needle);
  WCHAR *haystack = text;
  WCHAR *needleBuf =
      (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (needleLen + 1) * sizeof(WCHAR));
  if (!needleBuf) {
    HeapFree(GetProcessHeap(), 0, text);
    return FALSE;
  }
  StringCchCopyW(needleBuf, needleLen + 1, needle);

  if (!matchCase) {
    CharLowerBuffW(haystack, len);
    CharLowerBuffW(needleBuf, (DWORD)needleLen);
  }

  if (startPos > (DWORD)len)
    startPos = (DWORD)len;

  WCHAR *found = NULL;
  if (searchDown) {
    found = wcsstr(haystack + startPos, needleBuf);
    if (!found && startPos > 0) {
      found = wcsstr(haystack, needleBuf);
    }
  } else {
    WCHAR *p = haystack;
    while ((p = wcsstr(p, needleBuf)) != NULL) {
      DWORD idx = (DWORD)(p - haystack);
      if (idx < startPos) {
        found = p;
        p++;
      } else {
        break;
      }
    }
    if (!found && startPos < (DWORD)len) {
      p = haystack + startPos;
      while ((p = wcsstr(p, needleBuf)) != NULL) {
        found = p;
        p++;
      }
    }
  }

  BOOL result = FALSE;
  if (found) {
    DWORD pos = (DWORD)(found - haystack);
    *outStart = pos;
    *outEnd = pos + (DWORD)needleLen;
    result = TRUE;
  }

  HeapFree(GetProcessHeap(), 0, text);
  HeapFree(GetProcessHeap(), 0, needleBuf);
  return result;
}

static int ReplaceAllOccurrences(HWND hwndEdit, const WCHAR *needle,
                                 const WCHAR *replacement, BOOL matchCase) {
  if (!needle || needle[0] == L'\0')
    return 0;

  WCHAR *text = NULL;
  int len = 0;
  if (!GetEditText(hwndEdit, &text, &len))
    return 0;

  size_t needleLen = wcslen(needle);
  size_t replLen = replacement ? wcslen(replacement) : 0;

  WCHAR *searchBuf =
      (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
  WCHAR *needleBuf =
      (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (needleLen + 1) * sizeof(WCHAR));
  if (!searchBuf || !needleBuf) {
    HeapFree(GetProcessHeap(), 0, text);
    if (searchBuf)
      HeapFree(GetProcessHeap(), 0, searchBuf);
    if (needleBuf)
      HeapFree(GetProcessHeap(), 0, needleBuf);
    return 0;
  }
  StringCchCopyW(searchBuf, len + 1, text);
  StringCchCopyW(needleBuf, needleLen + 1, needle);

  if (!matchCase) {
    CharLowerBuffW(searchBuf, len);
    CharLowerBuffW(needleBuf, (DWORD)needleLen);
  }

  int count = 0;
  WCHAR *p = searchBuf;
  while ((p = wcsstr(p, needleBuf)) != NULL) {
    count++;
    p += needleLen;
  }
  if (count == 0) {
    HeapFree(GetProcessHeap(), 0, text);
    HeapFree(GetProcessHeap(), 0, searchBuf);
    HeapFree(GetProcessHeap(), 0, needleBuf);
    return 0;
  }

  size_t newLen =
      (size_t)len - (size_t)count * needleLen + (size_t)count * replLen;
  WCHAR *result =
      (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (newLen + 1) * sizeof(WCHAR));
  if (!result) {
    HeapFree(GetProcessHeap(), 0, text);
    HeapFree(GetProcessHeap(), 0, searchBuf);
    HeapFree(GetProcessHeap(), 0, needleBuf);
    return 0;
  }

  WCHAR *dst = result;
  WCHAR *searchCur = searchBuf;
  WCHAR *origCur = text;
  while ((p = wcsstr(searchCur, needleBuf)) != NULL) {
    size_t delta = (size_t)(p - searchCur);
    CopyMemory(dst, origCur, delta * sizeof(WCHAR));
    dst += delta;
    origCur += delta;
    searchCur += delta;

    if (replLen) {
      CopyMemory(dst, replacement, replLen * sizeof(WCHAR));
      dst += replLen;
    }
    origCur += needleLen;
    searchCur += needleLen;
  }
  size_t tail = wcslen(origCur);
  CopyMemory(dst, origCur, tail * sizeof(WCHAR));
  dst += tail;
  *dst = L'\0';

  SetWindowTextW(hwndEdit, result);
  HeapFree(GetProcessHeap(), 0, text);
  HeapFree(GetProcessHeap(), 0, searchBuf);
  HeapFree(GetProcessHeap(), 0, needleBuf);
  HeapFree(GetProcessHeap(), 0, result);
  SendMessageW(hwndEdit, EM_SETMODIFY, TRUE, 0);
  TabData *tab = GetCurrentTab();
  if (tab)
    tab->modified = TRUE;
  UpdateTitle(g_app.hwndMain);
  return count;
}

static void UpdateTitle(HWND hwnd) {
  TabData *tab = GetCurrentTab();
  if (!tab) {
    SetWindowTextW(hwnd, APP_TITLE);
    return;
  }

  WCHAR name[MAX_PATH_BUFFER];
  if (tab->currentPath[0]) {
    WCHAR *fileName = wcsrchr(tab->currentPath, L'\\');
    fileName = fileName ? fileName + 1 : tab->currentPath;
    StringCchCopyW(name, MAX_PATH_BUFFER, fileName);
  } else {
    StringCchCopyW(name, MAX_PATH_BUFFER, UNTITLED_NAME);
  }

  WCHAR title[MAX_PATH_BUFFER + 32];
  StringCchPrintfW(title, ARRAYSIZE(title), L"%s%s - %s",
                   (tab->modified ? L"*" : L""), name, APP_TITLE);
  SetWindowTextW(hwnd, title);
}

static void CreateTabControl(HWND hwnd) {
  g_app.hwndTab =
      CreateWindowExW(0, WC_TABCONTROLW, L"",
                      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_HOTTRACK |
                          TCS_OWNERDRAWFIXED,
                      0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)2, g_hInst, NULL);

  TabCtrl_SetPadding(g_app.hwndTab, 15, 3);

  HFONT hTabFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  SendMessageW(g_app.hwndTab, WM_SETFONT, (WPARAM)hTabFont, TRUE);
}

static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam, UINT_PTR uIdSubclass,
                                         DWORD_PTR dwRefData) {
  switch (msg) {
  case WM_MOUSEWHEEL:
    if (LOWORD(wParam) & MK_CONTROL) {
      int delta = GET_WHEEL_DELTA_WPARAM(wParam);
      if (delta > 0)
        SetZoom(g_app.hwndMain, g_app.zoomLevel + 10);
      else
        SetZoom(g_app.hwndMain, g_app.zoomLevel - 10);
      return 0;
    }
    break;
  case WM_NCDESTROY:
    RemoveWindowSubclass(hwnd, EditSubclassProc, uIdSubclass);
    break;
  }
  return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void CreateEditControlForTab(HWND hwnd, int index) {
  DWORD style = WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_WANTRETURN | ES_NOHIDESEL;
  if (!g_app.wordWrap) {
    style |= WS_HSCROLL | ES_AUTOHSCROLL;
  }

  g_app.tabs[index].hwndEdit =
      CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL, style, 0, 0, 0, 0, hwnd,
                      (HMENU)(UINT_PTR)(100 + index), g_hInst, NULL);
  if (g_app.tabs[index].hwndEdit && g_app.hFont) {
    SendMessageW(g_app.tabs[index].hwndEdit, WM_SETFONT, (WPARAM)g_app.hFont,
                 TRUE);
  }

  if (g_app.tabs[index].hwndEdit) {
    SetWindowSubclass(g_app.tabs[index].hwndEdit, EditSubclassProc, 0, 0);
  }

  SendMessageW(g_app.tabs[index].hwndEdit, EM_SETLIMITTEXT, 0, 0);
}

static void AddTab(HWND hwnd, LPCWSTR title, LPCWSTR path) {
  if (g_app.tabCount >= MAX_TABS)
    return;

  int index = g_app.tabCount++;
  g_app.tabs[index].modified = FALSE;
  g_app.tabs[index].encoding = ENC_UTF8;
  g_app.tabs[index].lineEnding = LE_CRLF;
  if (path) {
    StringCchCopyW(g_app.tabs[index].currentPath, MAX_PATH_BUFFER, path);
  } else {
    g_app.tabs[index].currentPath[0] = L'\0';
  }

  CreateEditControlForTab(hwnd, index);

  TCITEMW tie = {0};
  tie.mask = TCIF_TEXT;
  tie.pszText = (LPWSTR)title;
  TabCtrl_InsertItem(g_app.hwndTab, index, &tie);

  SwitchToTab(hwnd, index);
}

static void SwitchToTab(HWND hwnd, int index) {
  if (index < 0 || index >= g_app.tabCount)
    return;

  if (g_app.currentTabIdx >= 0 && g_app.currentTabIdx < g_app.tabCount) {
    ShowWindow(g_app.tabs[g_app.currentTabIdx].hwndEdit, SW_HIDE);
  }

  g_app.currentTabIdx = index;
  TabCtrl_SetCurSel(g_app.hwndTab, index);
  ShowWindow(g_app.tabs[index].hwndEdit, SW_SHOW);
  SetFocus(g_app.tabs[index].hwndEdit);

  UpdateLayout(hwnd);
  UpdateTitle(hwnd);
  UpdateStatusBar(hwnd);
}

static BOOL CloseTab(HWND hwnd, int index) {
  if (index < 0 || index >= g_app.tabCount)
    return FALSE;

  if (!PromptSaveChanges(hwnd, index))
    return FALSE;

  DestroyWindow(g_app.tabs[index].hwndEdit);
  TabCtrl_DeleteItem(g_app.hwndTab, index);

  for (int i = index; i < g_app.tabCount - 1; i++) {
    g_app.tabs[i] = g_app.tabs[i + 1];
  }
  g_app.tabCount--;

  if (g_app.tabCount == 0) {
    AddTab(hwnd, UNTITLED_NAME, NULL);
  } else {
    int nextIdx = (index >= g_app.tabCount) ? g_app.tabCount - 1 : index;
    g_app.currentTabIdx = -1; // Force switch
    SwitchToTab(hwnd, nextIdx);
  }
  return TRUE;
}

static void ShowTabContextMenu(HWND hwnd, int index) {
  HMENU hMenu = CreatePopupMenu();
  AppendMenuW(hMenu, MF_STRING, 55000, L"Close");
  AppendMenuW(hMenu, MF_STRING, 55001, L"Close Others");
  AppendMenuW(hMenu, MF_STRING, 55002, L"Close Tabs to the Right");

  POINT pt;
  GetCursorPos(&pt);
  int sel = TrackPopupMenu(
      hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x,
      pt.y, 0, hwnd, NULL);
  DestroyMenu(hMenu);

  if (sel == 55000) {
    CloseTab(hwnd, index);
  } else if (sel == 55001) {
    // Close others
    for (int i = g_app.tabCount - 1; i >= 0; i--) {
      if (i != index) {
        if (!CloseTab(hwnd, i))
          break;
      }
    }
  } else if (sel == 55002) {
    // Close right
    for (int i = g_app.tabCount - 1; i > index; i--) {
      if (!CloseTab(hwnd, i))
        break;
    }
  }
}

static void ToggleStatusBar(HWND hwnd, BOOL visible) {
  g_app.statusVisible = visible;
  if (visible) {
    if (!g_app.hwndStatus) {
      g_app.hwndStatus =
          CreateStatusWindowW(WS_CHILD | SBARS_SIZEGRIP, L"", hwnd, 2);
    }
    ShowWindow(g_app.hwndStatus, SW_SHOW);
  } else if (g_app.hwndStatus) {
    ShowWindow(g_app.hwndStatus, SW_HIDE);
  }
  UpdateLayout(hwnd);
  UpdateStatusBar(hwnd);
}

static void UpdateLayout(HWND hwnd) {
  RECT rc;
  GetClientRect(hwnd, &rc);

  int statusHeight = 0;
  if (g_app.statusVisible && g_app.hwndStatus) {
    SendMessageW(g_app.hwndStatus, WM_SIZE, 0, 0);
    RECT sbrc;
    GetWindowRect(g_app.hwndStatus, &sbrc);
    statusHeight = sbrc.bottom - sbrc.top;
    MoveWindow(g_app.hwndStatus, 0, rc.bottom - statusHeight, rc.right,
               statusHeight, TRUE);

    int parts[6];
    parts[0] = 150;
    parts[1] = 300;
    parts[2] = 400;
    parts[3] = rc.right - 280; // Zoom
    parts[4] = rc.right - 120; // Line Ending
    parts[5] = -1;             // Encoding
    SendMessageW(g_app.hwndStatus, SB_SETPARTS, 6, (LPARAM)parts);
  }

  int tabHeight = 0;
  if (g_app.hwndTab) {
    RECT trc;
    TabCtrl_GetItemRect(g_app.hwndTab, 0, &trc);
    tabHeight = trc.bottom - trc.top + 4;
    MoveWindow(g_app.hwndTab, 0, 0, rc.right, tabHeight, TRUE);
  }

  int lnWidth = 0;
  if (g_app.lineNumbersVisible && g_app.hwndLineNumbers) {
    lnWidth = 50;
    MoveWindow(g_app.hwndLineNumbers, 0, tabHeight, lnWidth,
               rc.bottom - statusHeight - tabHeight, TRUE);
    ShowWindow(g_app.hwndLineNumbers, SW_SHOW);
  } else if (g_app.hwndLineNumbers) {
    ShowWindow(g_app.hwndLineNumbers, SW_HIDE);
  }

  HWND hwndEdit = GetCurrentEdit();
  if (hwndEdit) {
    MoveWindow(hwndEdit, lnWidth, tabHeight, rc.right - lnWidth,
               rc.bottom - statusHeight - tabHeight, TRUE);
  }
}

static void ToggleLineNumbers(HWND hwnd, BOOL visible) {
  g_app.lineNumbersVisible = visible;
  CheckMenuItem(GetMenu(hwnd), IDM_VIEW_LINE_NUMBERS,
                MF_BYCOMMAND | (visible ? MF_CHECKED : MF_UNCHECKED));
  UpdateLayout(hwnd);
  if (g_app.hwndLineNumbers) {
    InvalidateRect(g_app.hwndLineNumbers, NULL, TRUE);
  }
}

static LRESULT CALLBACK LineNumberWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam) {
  switch (msg) {
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    HWND hwndEdit = GetCurrentEdit();
    if (hwndEdit) {
      RECT rc;
      GetClientRect(hwnd, &rc);
      FillRect(hdc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));

      HFONT hOldFont = NULL;
      if (g_app.hFont)
        hOldFont = (HFONT)SelectObject(hdc, g_app.hFont);

      SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
      SetBkMode(hdc, TRANSPARENT);

      int firstLine = (int)SendMessageW(hwndEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
      int lineCount = (int)SendMessageW(hwndEdit, EM_GETLINECOUNT, 0, 0);

      RECT rcEdit;
      SendMessageW(hwndEdit, EM_GETRECT, 0, (LPARAM)&rcEdit);
      int yOffset = rcEdit.top;

      // Get line height
      TEXTMETRIC tm;
      GetTextMetrics(hdc, &tm);
      int lineHeight = tm.tmHeight;

      // Calculate logical line number for the first visible line
      int logicalLine = 1;
      for (int i = 1; i <= firstLine; i++) {
        int prevLineIdx = (int)SendMessageW(hwndEdit, EM_LINEINDEX, i - 1, 0);
        int prevLineLen =
            (int)SendMessageW(hwndEdit, EM_LINELENGTH, prevLineIdx, 0);
        int currLineIdx = (int)SendMessageW(hwndEdit, EM_LINEINDEX, i, 0);
        if (currLineIdx > prevLineIdx + prevLineLen) {
          logicalLine++;
        }
      }

      for (int i = firstLine; i < lineCount; i++) {
        int y = yOffset + (i - firstLine) * lineHeight;
        if (y > rc.bottom)
          break;

        BOOL isNewLogical = FALSE;
        if (i == 0) {
          isNewLogical = TRUE;
        } else {
          int prevLineIdx = (int)SendMessageW(hwndEdit, EM_LINEINDEX, i - 1, 0);
          int prevLineLen =
              (int)SendMessageW(hwndEdit, EM_LINELENGTH, prevLineIdx, 0);
          int currLineIdx = (int)SendMessageW(hwndEdit, EM_LINEINDEX, i, 0);
          if (currLineIdx > prevLineIdx + prevLineLen) {
            isNewLogical = TRUE;
            if (i > firstLine)
              logicalLine++;
          }
        }

        if (isNewLogical) {
          WCHAR buf[16];
          StringCchPrintfW(buf, ARRAYSIZE(buf), L"%d", logicalLine);
          RECT rcLine = {0, y, rc.right - 5, y + lineHeight};
          DrawTextW(hdc, buf, -1, &rcLine,
                    DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        }
      }

      if (hOldFont)
        SelectObject(hdc, hOldFont);
    }
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static BOOL PromptSaveChanges(HWND hwnd, int index) {
  if (index < 0 || index >= g_app.tabCount)
    return TRUE;
  TabData *tab = &g_app.tabs[index];
  if (!tab->modified)
    return TRUE;

  WCHAR prompt[MAX_PATH_BUFFER + 64];
  const WCHAR *name = tab->currentPath[0] ? tab->currentPath : UNTITLED_NAME;
  StringCchPrintfW(prompt, ARRAYSIZE(prompt),
                   L"Do you want to save changes to %s?", name);
  int res =
      MessageBoxW(hwnd, prompt, APP_TITLE, MB_ICONQUESTION | MB_YESNOCANCEL);
  if (res == IDYES) {
    return DoFileSave(hwnd, index, FALSE);
  }
  return res == IDNO;
}

static BOOL LoadDocumentFromPath(HWND hwnd, int index, LPCWSTR path) {
  if (index < 0 || index >= g_app.tabCount)
    return FALSE;
  TabData *tab = &g_app.tabs[index];

  WCHAR *text = NULL;
  TextEncoding enc = ENC_UTF8;
  if (!LoadTextFile(hwnd, path, &text, NULL, &enc)) {
    return FALSE;
  }

  SetWindowTextW(tab->hwndEdit, text);
  HeapFree(GetProcessHeap(), 0, text);
  StringCchCopyW(tab->currentPath, ARRAYSIZE(tab->currentPath), path);
  tab->encoding = enc;

  // Detect line endings for the new tab
  tab->lineEnding = LE_CRLF;
  HWND hwndEdit = tab->hwndEdit;
  int len = GetWindowTextLengthW(hwndEdit);
  if (len > 0) {
    WCHAR *buffer =
        (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    if (buffer) {
      GetWindowTextW(hwndEdit, buffer, len + 1);
      for (int i = 0; i < len; i++) {
        if (buffer[i] == L'\n') {
          if (i > 0 && buffer[i - 1] == L'\r')
            tab->lineEnding = LE_CRLF;
          else
            tab->lineEnding = LE_LF;
          break;
        } else if (buffer[i] == L'\r') {
          if (i + 1 < len && buffer[i + 1] == L'\n')
            tab->lineEnding = LE_CRLF;
          else
            tab->lineEnding = LE_CR;
          break;
        }
      }
      HeapFree(GetProcessHeap(), 0, buffer);
    }
  }

  SendMessage(tab->hwndEdit, EM_SETMODIFY, FALSE, 0);
  tab->modified = FALSE;

  WCHAR *fileName = wcsrchr(tab->currentPath, L'\\');
  fileName = fileName ? fileName + 1 : tab->currentPath;
  TCITEMW tie = {0};
  tie.mask = TCIF_TEXT;
  tie.pszText = (LPWSTR)fileName;
  TabCtrl_SetItem(g_app.hwndTab, index, &tie);

  UpdateTitle(hwnd);
  UpdateStatusBar(hwnd);
  return TRUE;
}

static BOOL DoFileOpen(HWND hwnd) {
  WCHAR path[MAX_PATH_BUFFER] = L"";
  if (!OpenFileDialog(hwnd, path, ARRAYSIZE(path))) {
    return FALSE;
  }

  WCHAR *fileName = wcsrchr(path, L'\\');
  fileName = fileName ? fileName + 1 : path;
  AddTab(hwnd, fileName, path);
  return LoadDocumentFromPath(hwnd, g_app.currentTabIdx, path);
}

static BOOL DoFileSave(HWND hwnd, int index, BOOL saveAs) {
  if (index < 0 || index >= g_app.tabCount)
    return FALSE;
  TabData *tab = &g_app.tabs[index];

  WCHAR path[MAX_PATH_BUFFER];
  if (saveAs || tab->currentPath[0] == L'\0') {
    path[0] = L'\0';
    if (tab->currentPath[0]) {
      StringCchCopyW(path, ARRAYSIZE(path), tab->currentPath);
    }
    if (!SaveFileDialog(hwnd, path, ARRAYSIZE(path))) {
      return FALSE;
    }
    StringCchCopyW(tab->currentPath, ARRAYSIZE(tab->currentPath), path);

    WCHAR *fileName = wcsrchr(tab->currentPath, L'\\');
    fileName = fileName ? fileName + 1 : tab->currentPath;
    TCITEMW tie = {0};
    tie.mask = TCIF_TEXT;
    tie.pszText = (LPWSTR)fileName;
    TabCtrl_SetItem(g_app.hwndTab, index, &tie);
  } else {
    StringCchCopyW(path, ARRAYSIZE(path), tab->currentPath);
  }

  int len = GetWindowTextLengthW(tab->hwndEdit);
  WCHAR *buffer =
      (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
  if (!buffer)
    return FALSE;
  GetWindowTextW(tab->hwndEdit, buffer, len + 1);

  BOOL ok =
      SaveTextFile(hwnd, path, buffer, len, tab->encoding, tab->lineEnding);
  HeapFree(GetProcessHeap(), 0, buffer);
  if (ok) {
    SendMessageW(tab->hwndEdit, EM_SETMODIFY, FALSE, 0);
    tab->modified = FALSE;
    UpdateTitle(hwnd);
  }
  return ok;
}

static void DoFileNew(HWND hwnd) { AddTab(hwnd, UNTITLED_NAME, NULL); }

static void SetWordWrap(HWND hwnd, BOOL enabled) {
  if (g_app.wordWrap == enabled)
    return;
  g_app.wordWrap = enabled;

  for (int i = 0; i < g_app.tabCount; i++) {
    HWND edit = g_app.tabs[i].hwndEdit;
    WCHAR *text = NULL;
    int len = 0;
    if (!GetEditText(edit, &text, &len))
      continue;

    DWORD start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);

    DestroyWindow(edit);
    CreateEditControlForTab(hwnd, i);
    SetWindowTextW(g_app.tabs[i].hwndEdit, text);
    SendMessageW(g_app.tabs[i].hwndEdit, EM_SETSEL, start, end);
    HeapFree(GetProcessHeap(), 0, text);

    if (i != g_app.currentTabIdx) {
      ShowWindow(g_app.tabs[i].hwndEdit, SW_HIDE);
    } else {
      ShowWindow(g_app.tabs[i].hwndEdit, SW_SHOW);
    }
  }

  if (enabled) {
    EnableMenuItem(GetMenu(hwnd), IDM_EDIT_GOTO, MF_BYCOMMAND | MF_GRAYED);
  } else {
    EnableMenuItem(GetMenu(hwnd), IDM_EDIT_GOTO, MF_BYCOMMAND | MF_ENABLED);
  }
  UpdateTitle(hwnd);
  UpdateStatusBar(hwnd);
  UpdateLayout(hwnd);
}

static LPCWSTR GetEncodingName(TextEncoding enc) {
  switch (enc) {
  case ENC_UTF8:
    return L"UTF-8";
  case ENC_UTF16LE:
    return L"UTF-16 LE";
  case ENC_UTF16BE:
    return L"UTF-16 BE";
  case ENC_ANSI:
    return L"ANSI";
  default:
    return L"Unknown";
  }
}

static LPCWSTR GetLineEndingName(HWND hwndEdit) {
  (void)hwndEdit;
  TabData *tab = GetCurrentTab();
  if (!tab)
    return L"Windows (CRLF)";

  switch (tab->lineEnding) {
  case LE_CRLF:
    return L"Windows (CRLF)";
  case LE_LF:
    return L"Unix (LF)";
  case LE_CR:
    return L"Mac (CR)";
  default:
    return L"Windows (CRLF)";
  }
}

static void UpdateStatusBar(HWND hwnd) {
  (void)hwnd;
  if (!g_app.statusVisible || !g_app.hwndStatus)
    return;
  HWND hwndEdit = GetCurrentEdit();
  if (!hwndEdit)
    return;

  DWORD selStart = 0, selEnd = 0;
  SendMessageW(hwndEdit, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
  int line = (int)SendMessageW(hwndEdit, EM_LINEFROMCHAR, selStart, 0) + 1;
  int col =
      (int)(selStart - SendMessageW(hwndEdit, EM_LINEINDEX, line - 1, 0)) + 1;
  int length = GetWindowTextLengthW(hwndEdit);

  WCHAR buf[128];

  // Part 3: Zoom
  StringCchPrintfW(buf, ARRAYSIZE(buf), L"%d%%", g_app.zoomLevel);
  SendMessageW(g_app.hwndStatus, SB_SETTEXT, 3, (LPARAM)buf);

  // Part 0: Ln, Col
  StringCchPrintfW(buf, ARRAYSIZE(buf), L"Ln %d, Col %d", line, col);
  SendMessageW(g_app.hwndStatus, SB_SETTEXT, 0, (LPARAM)buf);

  // Part 1: Length
  StringCchPrintfW(buf, ARRAYSIZE(buf), L"Length: %d chars", length);
  SendMessageW(g_app.hwndStatus, SB_SETTEXT, 1, (LPARAM)buf);

  // Part 2: Tabs
  StringCchPrintfW(buf, ARRAYSIZE(buf), L"Tabs: %d", g_app.tabCount);
  SendMessageW(g_app.hwndStatus, SB_SETTEXT, 2, (LPARAM)buf);

  // Part 4: Line Endings
  SendMessageW(g_app.hwndStatus, SB_SETTEXT, 4,
               (LPARAM)GetLineEndingName(hwndEdit));

  // Part 5: Encoding
  TabData *tab = GetCurrentTab();
  SendMessageW(g_app.hwndStatus, SB_SETTEXT, 5,
               (LPARAM)GetEncodingName(tab ? tab->encoding : ENC_UTF8));
}

static void ShowFindDialog(HWND hwnd) {
  if (g_app.hFindDlg) {
    SetForegroundWindow(g_app.hFindDlg);
    return;
  }

  ZeroMemory(&g_app.find, sizeof(g_app.find));
  g_app.find.lStructSize = sizeof(FINDREPLACEW);
  g_app.find.hwndOwner = hwnd;
  g_app.find.lpstrFindWhat = g_app.findText;
  g_app.find.wFindWhatLen = ARRAYSIZE(g_app.findText);
  g_app.find.Flags = g_app.findFlags;

  g_app.hFindDlg = FindTextW(&g_app.find);
}

static void ShowTabSelector(HWND hwnd) {
  HMENU hMenu = CreatePopupMenu();
  for (int i = 0; i < g_app.tabCount; i++) {
    WCHAR name[MAX_PATH_BUFFER];
    TabData *tab = &g_app.tabs[i];
    if (tab->currentPath[0]) {
      WCHAR *fileName = wcsrchr(tab->currentPath, L'\\');
      fileName = fileName ? fileName + 1 : tab->currentPath;
      StringCchCopyW(name, MAX_PATH_BUFFER, fileName);
    } else {
      StringCchCopyW(name, MAX_PATH_BUFFER, UNTITLED_NAME);
    }

    if (tab->modified) {
      StringCchCatW(name, MAX_PATH_BUFFER, L" *");
    }

    UINT flags = MF_STRING;
    if (i == g_app.currentTabIdx)
      flags |= MF_CHECKED;

    AppendMenuW(hMenu, flags, 51000 + i, name);
  }

  AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(hMenu, MF_STRING, 52000, L"Close Current Tab");

  POINT pt;
  GetCursorPos(&pt);

  int sel = TrackPopupMenu(
      hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
      pt.x, pt.y, 0, hwnd, NULL);
  DestroyMenu(hMenu);

  if (sel >= 51000 && sel < 51000 + g_app.tabCount) {
    SwitchToTab(hwnd, sel - 51000);
  } else if (sel == 52000) {
    CloseTab(hwnd, g_app.currentTabIdx);
  }
}

static void ShowLineEndingSelector(HWND hwnd) {
  HMENU hMenu = CreatePopupMenu();
  AppendMenuW(hMenu, MF_STRING, 53000, L"Windows (CRLF)");
  AppendMenuW(hMenu, MF_STRING, 53001, L"Unix (LF)");
  AppendMenuW(hMenu, MF_STRING, 53002, L"Mac (CR)");

  TabData *tab = GetCurrentTab();
  if (tab) {
    CheckMenuItem(hMenu, 53000 + (int)tab->lineEnding,
                  MF_BYCOMMAND | MF_CHECKED);
  }

  POINT pt;
  GetCursorPos(&pt);
  int sel = TrackPopupMenu(
      hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
      pt.x, pt.y, 0, hwnd, NULL);
  DestroyMenu(hMenu);

  if (sel >= 53000 && sel <= 53002 && tab) {
    tab->lineEnding = (LineEnding)(sel - 53000);
    tab->modified = TRUE;
    UpdateTitle(hwnd);
    UpdateStatusBar(hwnd);
  }
}

static void ShowEncodingSelector(HWND hwnd) {
  HMENU hMenu = CreatePopupMenu();
  AppendMenuW(hMenu, MF_STRING, 54000, L"UTF-8");
  AppendMenuW(hMenu, MF_STRING, 54001, L"Windows-1252 (ANSI)");
  AppendMenuW(hMenu, MF_STRING, 54002, L"UTF-16 LE");
  AppendMenuW(hMenu, MF_STRING, 54003, L"UTF-16 BE");

  TabData *tab = GetCurrentTab();
  if (tab) {
    if (tab->encoding == ENC_UTF8)
      CheckMenuItem(hMenu, 54000, MF_BYCOMMAND | MF_CHECKED);
    else if (tab->encoding == ENC_ANSI)
      CheckMenuItem(hMenu, 54001, MF_BYCOMMAND | MF_CHECKED);
    else if (tab->encoding == ENC_UTF16LE)
      CheckMenuItem(hMenu, 54002, MF_BYCOMMAND | MF_CHECKED);
    else if (tab->encoding == ENC_UTF16BE)
      CheckMenuItem(hMenu, 54003, MF_BYCOMMAND | MF_CHECKED);
  }

  POINT pt;
  GetCursorPos(&pt);
  int sel = TrackPopupMenu(
      hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
      pt.x, pt.y, 0, hwnd, NULL);
  DestroyMenu(hMenu);

  if (sel >= 54000 && sel <= 54003 && tab) {
    switch (sel) {
    case 54000:
      tab->encoding = ENC_UTF8;
      break;
    case 54001:
      tab->encoding = ENC_ANSI;
      break;
    case 54002:
      tab->encoding = ENC_UTF16LE;
      break;
    case 54003:
      tab->encoding = ENC_UTF16BE;
      break;
    }
    tab->modified = TRUE;
    UpdateTitle(hwnd);
    UpdateStatusBar(hwnd);
  }
}

static void ShowReplaceDialog(HWND hwnd) {
  if (g_app.hReplaceDlg) {
    SetForegroundWindow(g_app.hReplaceDlg);
    return;
  }

  ZeroMemory(&g_app.find, sizeof(g_app.find));
  g_app.find.lStructSize = sizeof(FINDREPLACEW);
  g_app.find.hwndOwner = hwnd;
  g_app.find.lpstrFindWhat = g_app.findText;
  g_app.find.lpstrReplaceWith = g_app.replaceText;
  g_app.find.wFindWhatLen = ARRAYSIZE(g_app.findText);
  g_app.find.wReplaceWithLen = ARRAYSIZE(g_app.replaceText);
  g_app.find.Flags = g_app.findFlags;

  g_app.hReplaceDlg = ReplaceTextW(&g_app.find);
}

static BOOL DoFindNext(BOOL reverse) {
  if (g_app.findText[0] == L'\0') {
    ShowFindDialog(g_app.hwndMain);
    return FALSE;
  }

  HWND hwndEdit = GetCurrentEdit();
  if (!hwndEdit)
    return FALSE;

  DWORD start = 0, end = 0;
  SendMessageW(hwndEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
  BOOL matchCase = (g_app.findFlags & FR_MATCHCASE) != 0;
  BOOL down = (g_app.findFlags & FR_DOWN) != 0;
  if (reverse)
    down = !down;
  DWORD searchStart = down ? end : start;
  DWORD outStart = 0, outEnd = 0;
  if (FindInEdit(hwndEdit, g_app.findText, matchCase, down, searchStart,
                 &outStart, &outEnd)) {
    SendMessageW(hwndEdit, EM_SETSEL, outStart, outEnd);
    SendMessageW(hwndEdit, EM_SCROLLCARET, 0, 0);
    return TRUE;
  }
  MessageBoxW(g_app.hwndMain, L"Cannot find the text.", APP_TITLE,
              MB_ICONINFORMATION);
  return FALSE;
}

static INT_PTR CALLBACK GoToDlgProc(HWND dlg, UINT msg, WPARAM wParam,
                                    LPARAM lParam) {
  (void)lParam;
  switch (msg) {
  case WM_INITDIALOG: {
    SetDlgItemInt(dlg, IDC_GOTO_EDIT, 1, FALSE);
    HWND edit = GetDlgItem(dlg, IDC_GOTO_EDIT);
    SendMessageW(edit, EM_SETLIMITTEXT, 10, 0);
    return TRUE;
  }
  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IDOK: {
      BOOL ok = FALSE;
      UINT line = GetDlgItemInt(dlg, IDC_GOTO_EDIT, &ok, FALSE);
      if (!ok || line == 0) {
        MessageBoxW(dlg, L"Enter a valid line number.", APP_TITLE,
                    MB_ICONWARNING);
        return TRUE;
      }
      int maxLine = (int)SendMessageW(GetCurrentEdit(), EM_GETLINECOUNT, 0, 0);
      if ((int)line > maxLine)
        line = (UINT)maxLine;
      int charIndex =
          (int)SendMessageW(GetCurrentEdit(), EM_LINEINDEX, line - 1, 0);
      if (charIndex >= 0) {
        SendMessageW(GetCurrentEdit(), EM_SETSEL, charIndex, charIndex);
        SendMessageW(GetCurrentEdit(), EM_SCROLLCARET, 0, 0);
      }
      EndDialog(dlg, IDOK);
      return TRUE;
    }
    case IDCANCEL:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
    }
    break;
  }
  return FALSE;
}

static void DoSelectFont(HWND hwnd) {
  CHOOSEFONTW cf = {0};
  LOGFONTW lf;

  if (g_app.hFont) {
    GetObjectW(g_app.hFont, sizeof(LOGFONTW), &lf);
  } else {
    GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(LOGFONTW), &lf);
  }

  cf.lStructSize = sizeof(cf);
  cf.hwndOwner = hwnd;
  cf.lpLogFont = &lf;
  cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;

  if (ChooseFontW(&cf)) {
    if (g_app.hFont)
      DeleteObject(g_app.hFont);

    // Store base font
    g_app.baseFont = lf;
    g_app.zoomLevel = 100; // Reset zoom on font change

    g_app.hFont = CreateFontIndirectW(&lf);
    for (int i = 0; i < g_app.tabCount; i++) {
      SendMessageW(g_app.tabs[i].hwndEdit, WM_SETFONT, (WPARAM)g_app.hFont,
                   TRUE);
    }
    UpdateStatusBar(hwnd);
  }
}

static void InsertTimeDate(HWND hwnd) {
  (void)hwnd;
  SYSTEMTIME st;
  GetLocalTime(&st);
  WCHAR date[64], time[64], stamp[128];
  GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, date,
                 ARRAYSIZE(date));
  GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, time,
                 ARRAYSIZE(time));
  StringCchPrintfW(stamp, ARRAYSIZE(stamp), L"%s %s", time, date);
  SendMessageW(GetCurrentEdit(), EM_REPLACESEL, TRUE, (LPARAM)stamp);
}

static void HandleFindReplace(LPFINDREPLACEW lpfr) {
  if (lpfr->Flags & FR_DIALOGTERM) {
    g_app.hFindDlg = NULL;
    g_app.hReplaceDlg = NULL;
    return;
  }

  g_app.findFlags = lpfr->Flags;
  if (lpfr->lpstrFindWhat && lpfr->lpstrFindWhat[0]) {
    StringCchCopyW(g_app.findText, ARRAYSIZE(g_app.findText),
                   lpfr->lpstrFindWhat);
  }
  if (lpfr->lpstrReplaceWith) {
    StringCchCopyW(g_app.replaceText, ARRAYSIZE(g_app.replaceText),
                   lpfr->lpstrReplaceWith);
  }

  BOOL matchCase = (lpfr->Flags & FR_MATCHCASE) != 0;
  BOOL down = (lpfr->Flags & FR_DOWN) != 0;
  HWND hwndEdit = GetCurrentEdit();
  if (!hwndEdit)
    return;

  if (lpfr->Flags & FR_FINDNEXT) {
    DWORD start = 0, end = 0;
    SendMessageW(hwndEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    DWORD searchStart = down ? end : start;
    DWORD outStart = 0, outEnd = 0;
    if (FindInEdit(hwndEdit, g_app.findText, matchCase, down, searchStart,
                   &outStart, &outEnd)) {
      SendMessageW(hwndEdit, EM_SETSEL, outStart, outEnd);
      SendMessageW(hwndEdit, EM_SCROLLCARET, 0, 0);
    } else {
      MessageBoxW(g_app.hwndMain, L"Cannot find the text.", APP_TITLE,
                  MB_ICONINFORMATION);
    }
  } else if (lpfr->Flags & FR_REPLACE) {
    DWORD start = 0, end = 0;
    SendMessageW(hwndEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    DWORD outStart = 0, outEnd = 0;
    if (FindInEdit(hwndEdit, g_app.findText, matchCase, down, start, &outStart,
                   &outEnd)) {
      SendMessageW(hwndEdit, EM_SETSEL, outStart, outEnd);
      SendMessageW(hwndEdit, EM_REPLACESEL, TRUE, (LPARAM)g_app.replaceText);
      SendMessageW(hwndEdit, EM_SCROLLCARET, 0, 0);
      TabData *tab = GetCurrentTab();
      if (tab)
        tab->modified = TRUE;
      UpdateTitle(g_app.hwndMain);
    } else {
      MessageBoxW(g_app.hwndMain, L"Cannot find the text.", APP_TITLE,
                  MB_ICONINFORMATION);
    }
  } else if (lpfr->Flags & FR_REPLACEALL) {
    int replaced = ReplaceAllOccurrences(hwndEdit, g_app.findText,
                                         g_app.replaceText, matchCase);
    WCHAR msg[64];
    StringCchPrintfW(msg, ARRAYSIZE(msg), L"Replaced %d occurrence%s.",
                     replaced, replaced == 1 ? L"" : L"s");
    MessageBoxW(g_app.hwndMain, msg, APP_TITLE, MB_OK | MB_ICONINFORMATION);
  }
}

static void UpdateMenuStates(HWND hwnd) {
  HMENU menu = GetMenu(hwnd);
  if (!menu)
    return;

  UINT wrapState = g_app.wordWrap ? MF_CHECKED : MF_UNCHECKED;
  UINT statusState = g_app.statusVisible ? MF_CHECKED : MF_UNCHECKED;
  CheckMenuItem(menu, IDM_FORMAT_WORD_WRAP, MF_BYCOMMAND | wrapState);
  CheckMenuItem(menu, IDM_VIEW_STATUS_BAR, MF_BYCOMMAND | statusState);

  BOOL canGoTo = !g_app.wordWrap;
  EnableMenuItem(menu, IDM_EDIT_GOTO,
                 MF_BYCOMMAND | (canGoTo ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(menu, IDM_VIEW_STATUS_BAR, MF_BYCOMMAND | MF_ENABLED);

  BOOL modified = FALSE;
  TabData *tab = GetCurrentTab();
  if (tab)
    modified = (SendMessageW(tab->hwndEdit, EM_GETMODIFY, 0, 0) != 0);
  EnableMenuItem(menu, IDM_FILE_SAVE,
                 MF_BYCOMMAND | (modified ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(menu, IDM_FILE_CLOSE_TAB,
                 MF_BYCOMMAND | (g_app.tabCount > 0 ? MF_ENABLED : MF_GRAYED));
}

static void HandleCommand(HWND hwnd, WPARAM wParam, LPARAM lParam) {
  (void)lParam;
  switch (LOWORD(wParam)) {
  case IDM_FILE_NEW:
    if (g_app.tabCount > 0 && !PromptSaveChanges(hwnd, g_app.currentTabIdx))
      return;
    if (g_app.tabCount > 0) {
      TabData *tab = GetCurrentTab();
      SetWindowTextW(tab->hwndEdit, L"");
      tab->currentPath[0] = L'\0';
      tab->encoding = ENC_UTF8;
      SendMessageW(tab->hwndEdit, EM_SETMODIFY, FALSE, 0);
      tab->modified = FALSE;
      TCITEMW tie = {0};
      tie.mask = TCIF_TEXT;
      tie.pszText = (LPWSTR)UNTITLED_NAME;
      TabCtrl_SetItem(g_app.hwndTab, g_app.currentTabIdx, &tie);
      UpdateTitle(hwnd);
      UpdateStatusBar(hwnd);
    } else {
      DoFileNew(hwnd);
    }
    break;
  case IDM_FILE_NEW_TAB:
    DoFileNew(hwnd);
    break;
  case IDM_FILE_OPEN:
    DoFileOpen(hwnd);
    break;
  case IDM_FILE_SAVE:
    DoFileSave(hwnd, g_app.currentTabIdx, FALSE);
    break;
  case IDM_FILE_SAVE_AS:
    DoFileSave(hwnd, g_app.currentTabIdx, TRUE);
    break;
  case IDM_FILE_CLOSE_TAB:
    CloseTab(hwnd, g_app.currentTabIdx);
    break;
  case IDM_FILE_PAGE_SETUP:
  case IDM_FILE_PRINT:
    MessageBoxW(hwnd, L"Printing is not implemented in retropad.", APP_TITLE,
                MB_ICONINFORMATION);
    break;
  case IDM_FILE_EXIT:
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    break;

  case IDM_EDIT_UNDO:
    SendMessageW(GetCurrentEdit(), EM_UNDO, 0, 0);
    break;
  case IDM_EDIT_CUT:
    SendMessageW(GetCurrentEdit(), WM_CUT, 0, 0);
    break;
  case IDM_EDIT_COPY:
    SendMessageW(GetCurrentEdit(), WM_COPY, 0, 0);
    break;
  case IDM_EDIT_PASTE:
    SendMessageW(GetCurrentEdit(), WM_PASTE, 0, 0);
    break;
  case IDM_EDIT_DELETE:
    SendMessageW(GetCurrentEdit(), WM_CLEAR, 0, 0);
    break;
  case IDM_EDIT_FIND:
    ShowFindDialog(hwnd);
    break;
  case IDM_EDIT_FIND_NEXT:
    DoFindNext(FALSE);
    break;
  case IDM_EDIT_REPLACE:
    ShowReplaceDialog(hwnd);
    break;
  case IDM_EDIT_GOTO:
    if (g_app.wordWrap) {
      MessageBoxW(hwnd, L"Go To is unavailable when Word Wrap is on.",
                  APP_TITLE, MB_ICONINFORMATION);
    } else {
      DialogBoxW(g_hInst, MAKEINTRESOURCE(IDD_GOTO), hwnd, GoToDlgProc);
    }
    break;
  case IDM_EDIT_SELECT_ALL:
    SendMessageW(GetCurrentEdit(), EM_SETSEL, 0, -1);
    break;
  case IDM_EDIT_TIME_DATE:
    InsertTimeDate(hwnd);
    break;

  case IDM_FORMAT_WORD_WRAP:
    SetWordWrap(hwnd, !g_app.wordWrap);
    break;
  case IDM_FORMAT_FONT:
    DoSelectFont(hwnd);
    break;

  case IDM_VIEW_STATUS_BAR:
    ToggleStatusBar(hwnd, !g_app.statusVisible);
    break;
  case IDM_VIEW_LINE_NUMBERS:
    ToggleLineNumbers(hwnd, !g_app.lineNumbersVisible);
    break;
  case IDM_VIEW_ZOOM_IN:
    SetZoom(hwnd, g_app.zoomLevel + 10);
    break;
  case IDM_VIEW_ZOOM_OUT:
    SetZoom(hwnd, g_app.zoomLevel - 10);
    break;
  case IDM_VIEW_ZOOM_RESTORE:
    SetZoom(hwnd, 100);
    break;

  case IDM_HELP_VIEW_HELP:
    MessageBoxW(hwnd, L"No help file is available for retropad.", APP_TITLE,
                MB_ICONINFORMATION);
    break;
  case IDM_HELP_ABOUT:
    DialogBoxW(g_hInst, MAKEINTRESOURCE(IDD_ABOUT), hwnd, AboutDlgProc);
    break;
  }
}

static INT_PTR CALLBACK AboutDlgProc(HWND dlg, UINT msg, WPARAM wParam,
                                     LPARAM lParam) {
  (void)lParam;
  switch (msg) {
  case WM_INITDIALOG:
    return TRUE;
  case WM_COMMAND:
    if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
      EndDialog(dlg, LOWORD(wParam));
      return TRUE;
    }
    break;
  }
  return FALSE;
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam) {
  if (msg == g_findMsg) {
    HandleFindReplace((LPFINDREPLACEW)lParam);
    return 0;
  }

  switch (msg) {
  case WM_CREATE: {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wcln = {0};
    wcln.cbSize = sizeof(wcln);
    wcln.lpfnWndProc = LineNumberWndProc;
    wcln.hInstance = g_hInst;
    wcln.lpszClassName = L"RETROPAD_LINENUMBERS";
    wcln.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassExW(&wcln);

    g_app.hwndLineNumbers =
        CreateWindowExW(0, L"RETROPAD_LINENUMBERS", NULL, WS_CHILD | WS_VISIBLE,
                        0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)3, g_hInst, NULL);

    CreateTabControl(hwnd);
    AddTab(hwnd, UNTITLED_NAME, NULL);
    ToggleStatusBar(hwnd, TRUE);
    ToggleLineNumbers(hwnd, FALSE);
    g_app.zoomLevel = 100;
    UpdateTitle(hwnd);
    UpdateStatusBar(hwnd);
    DragAcceptFiles(hwnd, TRUE);
    return 0;
  }
  case WM_DRAWITEM: {
    if (wParam == 2) { // Tab control
      LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
      TCITEMW tie = {0};
      WCHAR text[MAX_PATH_BUFFER];
      tie.mask = TCIF_TEXT;
      tie.pszText = (LPWSTR)text;
      tie.cchTextMax = ARRAYSIZE(text);
      TabCtrl_GetItem(dis->hwndItem, dis->itemID, &tie);

      // Draw background
      FillRect(dis->hDC, &dis->rcItem, GetSysColorBrush(COLOR_BTNFACE));

      // Draw text
      RECT rcText = dis->rcItem;
      rcText.left += 5;
      rcText.right -= 20; // Leave room for 'x'
      DrawTextW(dis->hDC, text, -1, &rcText,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

      // Draw 'x' button
      RECT rcClose = dis->rcItem;
      rcClose.left = rcClose.right - 18;
      rcClose.right -= 4;
      rcClose.top += 4;
      rcClose.bottom -= 4;

      if (dis->itemState & ODS_SELECTED) {
        SetTextColor(dis->hDC, RGB(0, 0, 0));
      } else {
        SetTextColor(dis->hDC, RGB(100, 100, 100));
      }

      DrawTextW(dis->hDC, L"x", -1, &rcClose,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);

      // Draw border if selected
      if (dis->itemState & ODS_SELECTED) {
        DrawEdge(dis->hDC, &dis->rcItem, EDGE_RAISED, BF_RECT);
      }

      return TRUE;
    }
    break;
  }
  case WM_SETFOCUS:
    if (GetCurrentEdit())
      SetFocus(GetCurrentEdit());
    return 0;
  case WM_SIZE:
    UpdateLayout(hwnd);
    UpdateStatusBar(hwnd);
    return 0;
  case WM_DROPFILES: {
    HDROP hDrop = (HDROP)wParam;
    WCHAR path[MAX_PATH_BUFFER];
    if (DragQueryFileW(hDrop, 0, path, ARRAYSIZE(path))) {
      WCHAR *fileName = wcsrchr(path, L'\\');
      fileName = fileName ? fileName + 1 : path;
      AddTab(hwnd, fileName, path);
      LoadDocumentFromPath(hwnd, g_app.currentTabIdx, path);
    }
    DragFinish(hDrop);
    return 0;
  }
  case WM_COMMAND: {
    HWND hwndEdit = GetCurrentEdit();
    if (HIWORD(wParam) == EN_CHANGE && (HWND)lParam == hwndEdit) {
      TabData *tab = GetCurrentTab();
      if (tab) {
        tab->modified = (SendMessageW(hwndEdit, EM_GETMODIFY, 0, 0) != 0);
        UpdateTitle(hwnd);
        UpdateStatusBar(hwnd);
      }
      return 0;
    } else if ((HIWORD(wParam) == EN_UPDATE || HIWORD(wParam) == EN_VSCROLL) &&
               (HWND)lParam == hwndEdit) {
      UpdateStatusBar(hwnd);
      if (g_app.hwndLineNumbers) {
        InvalidateRect(g_app.hwndLineNumbers, NULL, TRUE);
      }
      return 0;
    }
    HandleCommand(hwnd, wParam, lParam);
    return 0;
  }
  case WM_NOTIFY: {
    LPNMHDR nmhdr = (LPNMHDR)lParam;
    if (nmhdr->hwndFrom == g_app.hwndTab) {
      if (nmhdr->code == TCN_SELCHANGE) {
        int index = TabCtrl_GetCurSel(g_app.hwndTab);
        SwitchToTab(hwnd, index);
      } else if (nmhdr->code == NM_CLICK || nmhdr->code == NM_RCLICK) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(g_app.hwndTab, &pt);
        TCHITTESTINFO hti = {0};
        hti.pt = pt;
        int index = TabCtrl_HitTest(g_app.hwndTab, &hti);
        if (index != -1) {
          if (nmhdr->code == NM_CLICK) {
            RECT rcItem;
            TabCtrl_GetItemRect(g_app.hwndTab, index, &rcItem);
            RECT rcClose = rcItem;
            rcClose.left = rcClose.right - 20;
            if (PtInRect(&rcClose, pt)) {
              CloseTab(hwnd, index);
              return 0;
            }
          } else {
            ShowTabContextMenu(hwnd, index);
          }
        }
      }
    } else if (nmhdr->hwndFrom == g_app.hwndStatus && nmhdr->code == NM_CLICK) {
      LPNMMOUSE lpnmm = (LPNMMOUSE)lParam;
      if (lpnmm->dwItemSpec == 2) { // Tabs part
        ShowTabSelector(hwnd);
      } else if (lpnmm->dwItemSpec == 4) { // Line endings part
        ShowLineEndingSelector(hwnd);
      } else if (lpnmm->dwItemSpec == 5) { // Encoding part
        ShowEncodingSelector(hwnd);
      }
    }
    break;
  }
  case WM_INITMENUPOPUP:
    UpdateMenuStates(hwnd);
    return 0;
  case WM_CLOSE:
    for (int i = g_app.tabCount - 1; i >= 0; i--) {
      if (!PromptSaveChanges(hwnd, i))
        return 0;
    }
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  case WM_MOUSEWHEEL:
    if (wParam & MK_CONTROL) {
      int delta = GET_WHEEL_DELTA_WPARAM(wParam);
      if (delta > 0)
        SetZoom(hwnd, g_app.zoomLevel + 10);
      else
        SetZoom(hwnd, g_app.zoomLevel - 10);
      return 0;
    }
    break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void SetZoom(HWND hwnd, int level) {
  if (level < 10)
    level = 10;
  if (level > 500)
    level = 500;
  g_app.zoomLevel = level;

  // Recreate font with new size
  if (g_app.hFont) {
    DeleteObject(g_app.hFont);
  }

  LOGFONTW lf = g_app.baseFont;
  // Calculate new height. lfHeight is usually negative (pixels).
  // If positive, it's cell height.
  // We scale it.
  lf.lfHeight = (LONG)(g_app.baseFont.lfHeight * (double)level / 100.0);
  // Ensure at least 1 pixel height to avoid errors, though <10% zoom check
  // handles most.
  if (lf.lfHeight == 0)
    lf.lfHeight = (g_app.baseFont.lfHeight > 0) ? 1 : -1;

  g_app.hFont = CreateFontIndirectW(&lf);

  for (int i = 0; i < g_app.tabCount; i++) {
    SendMessageW(g_app.tabs[i].hwndEdit, WM_SETFONT, (WPARAM)g_app.hFont, TRUE);
  }
  UpdateStatusBar(hwnd);
  if (g_app.hwndLineNumbers) {
    InvalidateRect(g_app.hwndLineNumbers, NULL, TRUE);
  }
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                      LPWSTR lpCmdLine, int nCmdShow) {
  (void)hPrevInstance;
  (void)lpCmdLine;

  g_hInst = hInstance;
  g_findMsg = RegisterWindowMessageW(FINDMSGSTRINGW);
  g_app.wordWrap = FALSE;
  g_app.statusVisible = TRUE;
  g_app.statusBeforeWrap = TRUE;
  g_app.findFlags = FR_DOWN;
  g_app.zoomLevel = 100;

  // Initialize base font
  HFONT hDefFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  GetObjectW(hDefFont, sizeof(LOGFONTW), &g_app.baseFont);
  g_app.hFont = CreateFontIndirectW(&g_app.baseFont);

  WNDCLASSEXW wc = {0};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = MainWndProc;
  wc.hInstance = hInstance;
  wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_RETROPAD));
  wc.hIconSm = wc.hIcon;
  wc.hCursor = LoadCursorW(NULL, IDC_IBEAM);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = L"RETROPAD_WINDOW";
  wc.lpszMenuName = MAKEINTRESOURCE(IDC_RETROPAD);

  if (!RegisterClassExW(&wc)) {
    MessageBoxW(NULL, L"Failed to register window class.", APP_TITLE,
                MB_ICONERROR);
    return 0;
  }

  HWND hwnd =
      CreateWindowExW(0, wc.lpszClassName, APP_TITLE, WS_OVERLAPPEDWINDOW,
                      CW_USEDEFAULT, CW_USEDEFAULT, DEFAULT_WIDTH,
                      DEFAULT_HEIGHT, NULL, NULL, hInstance, NULL);
  if (!hwnd) {
    MessageBoxW(NULL, L"Failed to create main window.", APP_TITLE,
                MB_ICONERROR);
    return 0;
  }

  g_app.hwndMain = hwnd;
  ShowWindow(hwnd, nCmdShow);
  UpdateWindow(hwnd);

  HACCEL accel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCE(IDC_RETROPAD));

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0)) {
    if (!accel || !TranslateAcceleratorW(hwnd, accel, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  return (int)msg.wParam;
}
