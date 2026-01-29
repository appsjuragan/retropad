// File loading and saving helpers for retropad
#pragma once

#include <windows.h>

typedef enum TextEncoding {
  ENC_UTF8 = 1,
  ENC_UTF16LE = 2,
  ENC_UTF16BE = 3,
  ENC_ANSI = 4
} TextEncoding;

typedef struct FileResult {
  WCHAR path[MAX_PATH];
  TextEncoding encoding;
} FileResult;

typedef enum LineEnding { LE_CRLF = 0, LE_LF = 1, LE_CR = 2 } LineEnding;

BOOL OpenFileDialog(HWND owner, WCHAR *pathOut, DWORD pathLen);
BOOL SaveFileDialog(HWND owner, WCHAR *pathOut, DWORD pathLen);

BOOL LoadTextFile(HWND owner, LPCWSTR path, WCHAR **textOut, size_t *lengthOut,
                  TextEncoding *encodingOut);
BOOL SaveTextFile(HWND owner, LPCWSTR path, LPCWSTR text, size_t length,
                  TextEncoding encoding, LineEnding lineEnding);
