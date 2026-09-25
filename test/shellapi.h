#ifndef FAKE_SHELLAPI_H
#define FAKE_SHELLAPI_H
typedef struct {
    DWORD cbSize; HWND hWnd; UINT uID, uFlags, uCallbackMessage; HICON hIcon; wchar_t szTip[128];
} NOTIFYICONDATAW;
#define NIM_ADD     0
#define NIM_MODIFY  1
#define NIM_DELETE  2
#define NIF_MESSAGE 1
#define NIF_ICON    2
#define NIF_TIP     4
BOOL Shell_NotifyIconW(DWORD, NOTIFYICONDATAW *);
#endif
