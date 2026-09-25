// wintray.c - Win32 implementation of wintray.h

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include "wintray.h"

#define WM_TRAY     (WM_APP + 1)     // notification icon callback message
#define ID_FIRST    1000             // first menu command ID
#define MAX_ITEMS   256
#define MAX_DEPTH   8                // max submenu nesting
#define MAX_MENUS   64               // submenus in total, so a menu that contains
                                     // itself can't create thousands of them
#define RETRY_TIMER 1

static struct wintray *g_tray;
static NOTIFYICONDATAW nid;
static HWND  hwnd;
static HMENU hmenu;
static int   icon_owned;             // nid.hIcon was loaded by us, not shared
static int   menu_open;
static int   update_pending;         // wintray_update() was called while the menu was open
static UINT  wm_taskbar_created;
static struct wintray_menu_item *items[MAX_ITEMS];  // indexed by command ID - ID_FIRST
static UINT  item_count;
static UINT  menu_count;

// Converts UTF-8 to UTF-16. Text that doesn't fit is cut at a character
// boundary; out is always null-terminated.
static void to_wide(const char *s, wchar_t *out, int size)
{
    out[0] = 0;
    if (!s)
        return;
    int bytes = lstrlenA(s);
    if (bytes > 3 * (size - 1))      // a UTF-16 unit never takes more than 3 bytes
        bytes = 3 * (size - 1);
    for (;;) {
        while (bytes > 0 && (s[bytes] & 0xC0) == 0x80)
            bytes--;                 // don't split a multi-byte character
        // Ask for the size first. What MultiByteToWideChar does with a buffer
        // that is too small differs between Windows and Wine.
        int need = bytes ? MultiByteToWideChar(CP_UTF8, 0, s, bytes, NULL, 0) : 0;
        if (need <= size - 1) {
            int n = need ? MultiByteToWideChar(CP_UTF8, 0, s, bytes, out, size - 1) : 0;
            out[n] = 0;
            return;
        }
        bytes -= need - (size - 1);  // a UTF-16 unit takes at least one byte
    }
}

static HMENU build_menu(struct wintray_menu_item *m, int depth)
{
    HMENU menu = CreatePopupMenu();
    for (; m && m->text; m++) {
        if (m->text[0] == '-' && m->text[1] == 0) {
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            continue;
        }
        wchar_t text[256];
        to_wide(m->text, text, 256);
        UINT flags = MF_STRING | (m->disabled ? MF_GRAYED : 0) | (m->checked ? MF_CHECKED : 0);
        if (m->submenu) {
            if (depth < MAX_DEPTH && menu_count < MAX_MENUS) {
                menu_count++;
                HMENU sub = build_menu(m->submenu, depth + 1);
                AppendMenuW(menu, flags | MF_POPUP, (UINT_PTR)sub, text);
            }
        } else if (item_count < MAX_ITEMS) {
            items[item_count] = m;
            AppendMenuW(menu, flags, ID_FIRST + item_count++, text);
        }
    }
    return menu;
}

// The module this code is in: the exe, or a DLL if wintray is built into one.
static HINSTANCE this_module(void)
{
    HMODULE m = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&g_tray, &m);
    return m;
}

static HICON load_icon(const struct wintray *t)
{
    int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
    HICON h = NULL;
    if (t->icon_id) {
        h = (HICON)LoadImageW(this_module(), MAKEINTRESOURCEW(t->icon_id),
                              IMAGE_ICON, cx, cy, 0);
    } else if (t->icon_filepath) {
        wchar_t path[MAX_PATH];
        to_wide(t->icon_filepath, path, MAX_PATH);
        h = (HICON)LoadImageW(NULL, path, IMAGE_ICON, cx, cy, LR_LOADFROMFILE);
    }
    icon_owned = (h != NULL);
    // Fall back to IDI_APPLICATION (32512). It's a shared icon: never destroy it.
    return h ? h : LoadIconW(NULL, MAKEINTRESOURCEW(32512));
}

// NIM_MODIFY fails when Explorer doesn't know the icon (first call, or after
// an Explorer restart), so fall back to NIM_ADD. If that fails too, Explorer
// is probably busy (e.g. right after login): retry every 2 seconds.
static void show_icon(void)
{
    if (Shell_NotifyIconW(NIM_MODIFY, &nid) || Shell_NotifyIconW(NIM_ADD, &nid))
        KillTimer(hwnd, RETRY_TIMER);
    else
        SetTimer(hwnd, RETRY_TIMER, 2000, NULL);
}

static void show_menu(void)
{
    if (menu_open)                   // icon clicked again while the menu is open
        return;
    POINT p = {0, 0};
    GetCursorPos(&p);
    // SetForegroundWindow before and WM_NULL after are both needed for tray
    // menus, see the remarks for TrackPopupMenu on MSDN.
    SetForegroundWindow(hwnd);
    menu_open = 1;
    UINT cmd = (UINT)TrackPopupMenu(hmenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                    p.x, p.y, 0, hwnd, NULL);
    menu_open = 0;
    if (!hwnd) {                     // wintray_exit() was called while the menu was open
        DestroyMenu(hmenu);
        hmenu = NULL;
        return;
    }
    PostMessageW(hwnd, WM_NULL, 0, 0);
    if (cmd >= ID_FIRST && cmd - ID_FIRST < item_count && items[cmd - ID_FIRST]->cb)
        items[cmd - ID_FIRST]->cb(items[cmd - ID_FIRST]);
    if (update_pending) {
        update_pending = 0;
        wintray_update(g_tray);
    }
}

// Runs on WM_DESTROY, so it also cleans up when the window is closed by
// someone else (taskkill without /f, an installer, ...).
static void cleanup(void)
{
    // Clear hwnd first, so wnd_proc() ignores clicks that come in while
    // Shell_NotifyIconW waits for Explorer.
    hwnd = NULL;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    update_pending = 0;
    if (menu_open) {
        EndMenu();                   // show_menu() frees the menu when it returns
    } else if (hmenu) {
        DestroyMenu(hmenu);
        hmenu = NULL;
    }
    if (nid.hIcon && icon_owned)
        DestroyIcon(nid.hIcon);
    nid.hIcon = NULL;
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!hwnd)                       // still in CreateWindowW, or closing
        return DefWindowProcW(w, msg, wp, lp);
    if (msg == WM_TRAY) {
        // A double-click gives DOWN, UP, DBLCLK, UP: skip the second UP. Timing
        // the clicks doesn't work if cb opens a dialog or takes a while.
        static int dblclk;
        UINT event = LOWORD(lp);
        if (event == WM_LBUTTONDOWN)
            dblclk = 0;
        else if (event == WM_LBUTTONDBLCLK)
            dblclk = 1;
        else if (event == WM_LBUTTONUP && dblclk)
            dblclk = 0;
        else if (event == WM_LBUTTONUP && g_tray->cb)
            g_tray->cb(g_tray);
        else if (event == WM_LBUTTONUP || event == WM_RBUTTONUP)
            show_menu();
        return 0;
    }
    if (msg == WM_TIMER && wp == RETRY_TIMER) {
        show_icon();
        return 0;
    }
    if (msg == wm_taskbar_created && msg != 0) {   // Explorer restarted
        show_icon();
        return 0;
    }
    if (msg == WM_DESTROY) {
        cleanup();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

struct wintray *wintray_get_instance(void) { return g_tray; }

int wintray_init(struct wintray *tray)
{
    if (!tray || hwnd)
        return -1;
    wm_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    // Hidden window for the icon messages. Not a message-only window, because
    // those don't receive broadcasts such as TaskbarCreated.
    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = this_module();
    wc.lpszClassName = L"WintrayWindow";
    // Register the class fresh: after a DLL is unloaded and loaded again, the
    // old registration still points to the old wnd_proc.
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (!RegisterClassW(&wc))
        return -1;
    hwnd = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
        return -1;
    // Otherwise an elevated process never gets TaskbarCreated (UIPI).
    ChangeWindowMessageFilterEx(hwnd, wm_taskbar_created, MSGFLT_ALLOW, NULL);

    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    wintray_update(tray);
    return 0;
}

int wintray_loop(int blocking)
{
    MSG msg;
    // NULL window filter, so WM_QUIT and thread timers come through too.
    if (blocking) {
        if (GetMessageW(&msg, NULL, 0, 0) <= 0)
            return -1;
    } else {
        if (!PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            return 0;
        if (msg.message == WM_QUIT)
            return -1;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
    return 0;
}

void wintray_update(struct wintray *tray)
{
    if (!hwnd || !tray)
        return;
    g_tray = tray;
    if (menu_open) {                 // can't destroy a menu that is on screen;
        update_pending = 1;          // show_menu() calls us again when it closes
        return;
    }

    if (hmenu)
        DestroyMenu(hmenu);          // destroys the submenus too
    item_count = 0;
    menu_count = 0;
    hmenu = build_menu(tray->menu, 0);

    HICON old = nid.hIcon;
    int old_owned = icon_owned;
    nid.hIcon = load_icon(tray);
    to_wide(tray->tooltip, nid.szTip, sizeof(nid.szTip) / sizeof(nid.szTip[0]));
    show_icon();
    if (old && old_owned)
        DestroyIcon(old);
}

void wintray_exit(void)
{
    if (hwnd)
        DestroyWindow(hwnd);         // the rest happens in WM_DESTROY
}
