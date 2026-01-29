// Text file load/save helpers with simple BOM detection for retropad.
#include "file_io.h"
#include <commdlg.h>
#include <stdlib.h>
#include <strsafe.h>


static TextEncoding DetectEncoding(const BYTE *data, DWORD size) {
  if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
    return ENC_UTF16LE;
  }
  if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
    return ENC_UTF16BE;
  }
  if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
    return ENC_UTF8;
  }
  // Assume UTF-8 if it converts cleanly, else ANSI
  int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCSTR)data,
                                 size, NULL, 0);
  return (wlen > 0) ? ENC_UTF8 : ENC_ANSI;
}

static BOOL DecodeToWide(const BYTE *data, DWORD size, TextEncoding encoding,
                         WCHAR **outText, size_t *outLength) {
  int chars = 0;
  WCHAR *buffer = NULL;

  switch (encoding) {
  case ENC_UTF16LE: {
    if (size < 2)
      return FALSE;
    DWORD byteOffset = (data[0] == 0xFF && data[1] == 0xFE) ? 2 : 0;
    DWORD wcharCount = (size - byteOffset) / 2;
    buffer = (WCHAR *)HeapAlloc(GetProcessHeap(), 0,
                                (wcharCount + 1) * sizeof(WCHAR));
    if (!buffer)
      return FALSE;
    CopyMemory(buffer, data + byteOffset, wcharCount * sizeof(WCHAR));
    buffer[wcharCount] = L'\0';
    chars = (int)wcharCount;
    break;
  }
  case ENC_UTF16BE: {
    if (size < 2)
      return FALSE;
    DWORD byteOffset = (data[0] == 0xFE && data[1] == 0xFF) ? 2 : 0;
    DWORD wcharCount = (size - byteOffset) / 2;
    buffer = (WCHAR *)HeapAlloc(GetProcessHeap(), 0,
                                (wcharCount + 1) * sizeof(WCHAR));
    if (!buffer)
      return FALSE;
    for (DWORD i = 0; i < wcharCount; ++i) {
      buffer[i] = (WCHAR)((data[byteOffset + i * 2] << 8) |
                          data[byteOffset + i * 2 + 1]);
    }
    buffer[wcharCount] = L'\0';
    chars = (int)wcharCount;
    break;
  }
  case ENC_UTF8: {
    DWORD byteOffset =
        (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
            ? 3
            : 0;
    chars = MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)(data + byteOffset),
                                size - byteOffset, NULL, 0);
    if (chars <= 0)
      return FALSE;
    buffer =
        (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (chars + 1) * sizeof(WCHAR));
    if (!buffer)
      return FALSE;
    MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)(data + byteOffset),
                        size - byteOffset, buffer, chars);
    buffer[chars] = L'\0';
    break;
  }
  case ENC_ANSI:
  default: {
    chars = MultiByteToWideChar(CP_ACP, 0, (LPCSTR)data, size, NULL, 0);
    if (chars <= 0)
      return FALSE;
    buffer =
        (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (chars + 1) * sizeof(WCHAR));
    if (!buffer)
      return FALSE;
    MultiByteToWideChar(CP_ACP, 0, (LPCSTR)data, size, buffer, chars);
    buffer[chars] = L'\0';
    break;
  }
  }

  *outText = buffer;
  if (outLength) {
    *outLength = (size_t)chars;
  }
  return TRUE;
}

BOOL LoadTextFile(HWND owner, LPCWSTR path, WCHAR **textOut, size_t *lengthOut,
                  TextEncoding *encodingOut) {
  *textOut = NULL;
  if (lengthOut)
    *lengthOut = 0;
  if (encodingOut)
    *encodingOut = ENC_UTF8;

  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    MessageBoxW(owner, L"Unable to open file.", L"retropad", MB_ICONERROR);
    return FALSE;
  }

  LARGE_INTEGER size = {0};
  if (!GetFileSizeEx(file, &size) || size.QuadPart > (LONGLONG)UINT_MAX) {
    CloseHandle(file);
    MessageBoxW(owner, L"Unsupported file size.", L"retropad", MB_ICONERROR);
    return FALSE;
  }

  DWORD bytes = (DWORD)size.QuadPart;
  BYTE *buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, bytes + 3);
  if (!buffer) {
    CloseHandle(file);
    MessageBoxW(owner, L"Out of memory.", L"retropad", MB_ICONERROR);
    return FALSE;
  }

  DWORD read = 0;
  BOOL ok = ReadFile(file, buffer, bytes, &read, NULL);
  CloseHandle(file);
  if (!ok) {
    HeapFree(GetProcessHeap(), 0, buffer);
    MessageBoxW(owner, L"Failed reading file.", L"retropad", MB_ICONERROR);
    return FALSE;
  }

  if (read == 0) {
    WCHAR *empty = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR));
    if (!empty) {
      HeapFree(GetProcessHeap(), 0, buffer);
      return FALSE;
    }
    empty[0] = L'\0';
    *textOut = empty;
    if (lengthOut)
      *lengthOut = 0;
    if (encodingOut)
      *encodingOut = ENC_UTF8;
    HeapFree(GetProcessHeap(), 0, buffer);
    return TRUE;
  }

  TextEncoding enc = DetectEncoding(buffer, read);
  WCHAR *text = NULL;
  size_t len = 0;
  if (!DecodeToWide(buffer, read, enc, &text, &len)) {
    HeapFree(GetProcessHeap(), 0, buffer);
    MessageBoxW(owner, L"Unable to decode file.", L"retropad", MB_ICONERROR);
    return FALSE;
  }

  HeapFree(GetProcessHeap(), 0, buffer);
  *textOut = text;
  if (lengthOut)
    *lengthOut = len;
  if (encodingOut)
    *encodingOut = enc;
  return TRUE;
}

static BOOL WriteUTF8WithBOM(HANDLE file, const WCHAR *text, size_t length) {
  static const BYTE bom[] = {0xEF, 0xBB, 0xBF};
  DWORD written = 0;
  if (!WriteFile(file, bom, sizeof(bom), &written, NULL)) {
    return FALSE;
  }
  int bytes =
      WideCharToMultiByte(CP_UTF8, 0, text, (int)length, NULL, 0, NULL, NULL);
  if (bytes <= 0)
    return FALSE;
  BYTE *buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, bytes);
  if (!buffer)
    return FALSE;
  WideCharToMultiByte(CP_UTF8, 0, text, (int)length, (LPSTR)buffer, bytes, NULL,
                      NULL);
  BOOL ok = WriteFile(file, buffer, bytes, &written, NULL);
  HeapFree(GetProcessHeap(), 0, buffer);
  return ok;
}

static BOOL WriteUTF16LE(HANDLE file, const WCHAR *text, size_t length) {
  static const BYTE bom[] = {0xFF, 0xFE};
  DWORD written = 0;
  if (!WriteFile(file, bom, sizeof(bom), &written, NULL)) {
    return FALSE;
  }
  return WriteFile(file, text, (DWORD)(length * sizeof(WCHAR)), &written, NULL);
}

static BOOL WriteANSI(HANDLE file, const WCHAR *text, size_t length) {
  int bytes =
      WideCharToMultiByte(CP_ACP, 0, text, (int)length, NULL, 0, NULL, NULL);
  if (bytes <= 0)
    return FALSE;
  BYTE *buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, bytes);
  if (!buffer)
    return FALSE;
  WideCharToMultiByte(CP_ACP, 0, text, (int)length, (LPSTR)buffer, bytes, NULL,
                      NULL);
  DWORD written = 0;
  BOOL ok = WriteFile(file, buffer, bytes, &written, NULL);
  HeapFree(GetProcessHeap(), 0, buffer);
  return ok;
}

static WCHAR *NormalizeLineEndings(LPCWSTR text, size_t length,
                                   LineEnding target, size_t *outLength) {
  // First pass: count required size
  size_t newLen = 0;
  for (size_t i = 0; i < length; i++) {
    if (text[i] == L'\r') {
      if (i + 1 < length && text[i + 1] == L'\n') {
        // CRLF
        if (target == LE_CRLF)
          newLen += 2;
        else
          newLen += 1; // LF or CR
        i++;
      } else {
        // lone CR
        newLen++;
      }
    } else if (text[i] == L'\n') {
      // lone LF
      if (target == LE_CRLF)
        newLen += 2;
      else
        newLen += 1;
    } else {
      newLen++;
    }
  }

  WCHAR *buffer =
      (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (newLen + 1) * sizeof(WCHAR));
  if (!buffer)
    return NULL;

  size_t j = 0;
  for (size_t i = 0; i < length; i++) {
    if (text[i] == L'\r') {
      if (i + 1 < length && text[i + 1] == L'\n') {
        if (target == LE_CRLF) {
          buffer[j++] = L'\r';
          buffer[j++] = L'\n';
        } else if (target == LE_LF) {
          buffer[j++] = L'\n';
        } else if (target == LE_CR) {
          buffer[j++] = L'\r';
        }
        i++;
      } else {
        if (target == LE_CRLF) {
          buffer[j++] = L'\r';
          buffer[j++] = L'\n';
        } else if (target == LE_LF) {
          buffer[j++] = L'\n';
        } else if (target == LE_CR) {
          buffer[j++] = L'\r';
        }
      }
    } else if (text[i] == L'\n') {
      if (target == LE_CRLF) {
        buffer[j++] = L'\r';
        buffer[j++] = L'\n';
      } else if (target == LE_LF) {
        buffer[j++] = L'\n';
      } else if (target == LE_CR) {
        buffer[j++] = L'\r';
      }
    } else {
      buffer[j++] = text[i];
    }
  }
  buffer[j] = L'\0';
  *outLength = j;
  return buffer;
}

BOOL SaveTextFile(HWND owner, LPCWSTR path, LPCWSTR text, size_t length,
                  TextEncoding encoding, LineEnding lineEnding) {
  size_t finalLen = 0;
  WCHAR *finalText = NormalizeLineEndings(text, length, lineEnding, &finalLen);
  if (!finalText) {
    MessageBoxW(owner, L"Out of memory during normalization.", L"retropad",
                MB_ICONERROR);
    return FALSE;
  }

  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    HeapFree(GetProcessHeap(), 0, finalText);
    MessageBoxW(owner, L"Unable to create file.", L"retropad", MB_ICONERROR);
    return FALSE;
  }

  BOOL ok = FALSE;
  switch (encoding) {
  case ENC_UTF16LE:
    ok = WriteUTF16LE(file, finalText, finalLen);
    break;
  case ENC_ANSI:
    ok = WriteANSI(file, finalText, finalLen);
    break;
  case ENC_UTF16BE:
    ok = WriteUTF8WithBOM(file, finalText, finalLen);
    encoding = ENC_UTF8;
    break;
  case ENC_UTF8:
  default:
    ok = WriteUTF8WithBOM(file, finalText, finalLen);
    break;
  }

  CloseHandle(file);
  HeapFree(GetProcessHeap(), 0, finalText);
  if (!ok) {
    MessageBoxW(owner, L"Failed writing file.", L"retropad", MB_ICONERROR);
  }
  return ok;
}

BOOL OpenFileDialog(HWND owner, WCHAR *pathOut, DWORD pathLen) {
  pathOut[0] = L'\0';
  OPENFILENAMEW ofn = {0};
  WCHAR filter[] = L"Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0\0";
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = pathOut;
  ofn.nMaxFile = pathLen;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
  ofn.lpstrDefExt = L"txt";
  return GetOpenFileNameW(&ofn);
}

BOOL SaveFileDialog(HWND owner, WCHAR *pathOut, DWORD pathLen) {
  OPENFILENAMEW ofn = {0};
  WCHAR filter[] = L"Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0\0";
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = pathOut;
  ofn.nMaxFile = pathLen;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  ofn.lpstrDefExt = L"txt";
  if (pathOut[0] == L'\0') {
    StringCchCopyW(pathOut, pathLen, L"*.txt");
  }
  return GetSaveFileNameW(&ofn);
}
