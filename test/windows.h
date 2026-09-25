// Minimal fake Win32 declarations, enough to compile wintray.c on Linux.
#ifndef FAKE_WINDOWS_H
#define FAKE_WINDOWS_H
#include <stddef.h>
#include <stdint.h>

typedef int BOOL;
typedef unsigned int UINT;
typedef unsigned int DWORD;
typedef intptr_t LRESULT;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef uintptr_t UINT_PTR;
typedef void *HANDLE;
typedef struct HWND__ *HWND;
typedef struct HMENU__ *HMENU;
typedef struct HICON__ *HICON;
typedef struct HINSTANCE__ *HINSTANCE;
typedef HINSTANCE HMODULE;
typedef const char *LPCSTR;
typedef const wchar_t *LPCWSTR;
#define CALLBACK
#define TRUE  1
#define FALSE 0
typedef struct { long x, y; } POINT;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG;
typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct {
    UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance;
    HICON hIcon; void *hCursor, *hbrBackground; LPCWSTR lpszMenuName, lpszClassName;
} WNDCLASSW;

#define WM_NULL          0x0000
#define WM_DESTROY       0x0002
#define WM_QUIT          0x0012
#define WM_TIMER         0x0113
#define WM_LBUTTONDOWN   0x0201
#define WM_LBUTTONUP     0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN   0x0204
#define WM_RBUTTONUP     0x0205
#define WM_APP           0x8000
#define LOWORD(l)        ((unsigned short)((uintptr_t)(l) & 0xFFFF))
#define MAKEINTRESOURCEW(i) ((LPCWSTR)(uintptr_t)(unsigned short)(i))
#define CP_UTF8          65001
#define MF_STRING        0x0000
#define MF_GRAYED        0x0001
#define MF_CHECKED       0x0008
#define MF_POPUP         0x0010
#define MF_SEPARATOR     0x0800
#define SM_CXSMICON      49
#define SM_CYSMICON      50
#define IMAGE_ICON       1
#define LR_LOADFROMFILE  0x0010
#define MAX_PATH         260
#define TPM_RIGHTBUTTON  0x0002
#define TPM_NONOTIFY     0x0080
#define TPM_RETURNCMD    0x0100
#define PM_REMOVE        0x0001
#define MSGFLT_ALLOW     1
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x2
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS       0x4

int lstrlenA(LPCSTR);
int MultiByteToWideChar(UINT, DWORD, LPCSTR, int, wchar_t *, int);
HMENU CreatePopupMenu(void);
BOOL AppendMenuW(HMENU, UINT, UINT_PTR, LPCWSTR);
BOOL DestroyMenu(HMENU);
int GetSystemMetrics(int);
HANDLE LoadImageW(HINSTANCE, LPCWSTR, UINT, int, int, UINT);
HICON LoadIconW(HINSTANCE, LPCWSTR);
BOOL DestroyIcon(HICON);
HMODULE GetModuleHandleW(LPCWSTR);
BOOL GetModuleHandleExW(DWORD, LPCWSTR, HMODULE *);
UINT_PTR SetTimer(HWND, UINT_PTR, UINT, void *);
BOOL KillTimer(HWND, UINT_PTR);
BOOL GetCursorPos(POINT *);
BOOL SetForegroundWindow(HWND);
BOOL TrackPopupMenu(HMENU, UINT, int, int, int, HWND, const void *);
BOOL PostMessageW(HWND, UINT, WPARAM, LPARAM);
BOOL EndMenu(void);
void PostQuitMessage(int);
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM);
UINT RegisterWindowMessageW(LPCWSTR);
unsigned short RegisterClassW(const WNDCLASSW *);
BOOL UnregisterClassW(LPCWSTR, HINSTANCE);
HWND CreateWindowW(LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, void *);
BOOL DestroyWindow(HWND);
BOOL ChangeWindowMessageFilterEx(HWND, UINT, DWORD, void *);
BOOL GetMessageW(MSG *, HWND, UINT, UINT);
BOOL PeekMessageW(MSG *, HWND, UINT, UINT, UINT);
BOOL TranslateMessage(const MSG *);
LRESULT DispatchMessageW(const MSG *);
#endif
