// wintray.c - Win32 implementation of wintray.h

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include "wintray.h"

#define WM_TRAY     (WM_APP + 1)     // notification icon callback message
#define ID_FIRST    1000             // first menu command ID
#define MAX_ITEMS   256
#define MAX_TEXT    256              // a menu item's text in UTF-16 units, with the final 0
#define MAX_DEPTH   8                // max submenu nesting
#define MAX_MENUS   256              // max submenus in total, in case a menu
                                     // contains itself
#define RETRY_TIMER 1
#define CLASS_NAME  L"WintrayWindow"

// wnd_proc() can run again inside TrackPopupMenu, Shell_NotifyIconW (it waits
// for Explorer) and the user's callbacks. Update the state below before
// calling them, and check it again after they return.
static struct wintray *g_tray;
static NOTIFYICONDATAW nid;
static HWND  hwnd;
static HWND  window;                 // the window wintray made, maybe gone since; see
                                     // still_there()
static int   closing;                // cleanup() is running
static int   notifying;              // inside Shell_NotifyIconW, which waits for Explorer
static HMENU hmenu;
static HICON own_icon;               // the icon in nid if we loaded it, else NULL
static int   menu_open;
static int   update_pending;         // wintray_update() was called while the menu was open
static UINT  wm_taskbar_created;
static struct wintray_menu_item *items[MAX_ITEMS];  // indexed by command ID - ID_FIRST
static struct {                      // each item as the menu shows it, besides its text
    void (*cb)(struct wintray_menu_item *);
    UINT flags;                      // MF_GRAYED also when a submenu above it is grayed out
} built[MAX_ITEMS];
static UINT  item_count;
static UINT  menu_count;

// Length of the valid UTF-8 character that starts at s, or 0 if none does.
static int utf8_len(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    unsigned c = u[0];
    int n = c < 0x80 ? 1 : c < 0xC2 ? 0 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : c < 0xF5 ? 4 : 0;
    // Second bytes outside lo..hi would give an overlong form, a surrogate or
    // more than U+10FFFF.
    unsigned lo = c == 0xE0 ? 0xA0 : c == 0xF0 ? 0x90 : 0x80;
    unsigned hi = c == 0xED ? 0x9F : c == 0xF4 ? 0x8F : 0xBF;
    if (n > 1 && (u[1] < lo || u[1] > hi))
        return 0;
    for (int i = 2; i < n; i++)
        if ((u[i] & 0xC0) != 0x80)
            return 0;
    return n;
}

// Converts UTF-8 to UTF-16. Text that doesn't fit is cut at a character
// boundary; out is always null-terminated.
//
// A UTF-16 unit takes 1 to 3 bytes. So text with fewer bytes than the buffer
// has room for always fits, and when text doesn't fit, dropping one byte per
// unit too many never drops more than needed.
static void to_wide(const char *s, wchar_t *out, int size)
{
    out[0] = 0;
    if (!s)
        return;
    int bytes = lstrlenA(s);
    if (bytes > 3 * (size - 1))
        bytes = 3 * (size - 1);
    for (;;) {
        // Don't cut inside a character: if a valid one starts up to 3 bytes
        // back and runs past the cut, cut before it. Bytes of invalid text
        // belong to no character, so a cut among them can stay.
        int cut = bytes;
        for (int k = 1; k <= 3 && k <= cut; k++)
            if (utf8_len(s + cut - k) > k)
                bytes = cut - k;
        if (!bytes)
            return;
        // What MultiByteToWideChar does with a buffer that is too small differs
        // between Windows and Wine, so ask for the size unless it surely fits.
        int need = bytes < size ? bytes : MultiByteToWideChar(CP_UTF8, 0, s, bytes, NULL, 0);
        if (need <= size - 1) {
            out[MultiByteToWideChar(CP_UTF8, 0, s, bytes, out, size - 1)] = 0;
            return;
        }
        bytes -= need - (size - 1);
    }
}

static UINT menu_flags(const struct wintray_menu_item *m)
{
    return MF_STRING | (m->disabled ? MF_GRAYED : 0) | (m->checked ? MF_CHECKED : 0);
}

// above is MF_GRAYED inside a grayed-out submenu, whose items can't be reached.
static HMENU build_menu(struct wintray_menu_item *m, int depth, UINT above)
{
    HMENU menu = CreatePopupMenu();
    for (; m && m->text; m++) {
        if (m->text[0] == '-' && m->text[1] == 0) {
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            continue;
        }
        wchar_t text[MAX_TEXT];
        to_wide(m->text, text, ARRAYSIZE(text));
        UINT flags = menu_flags(m);
        if (m->submenu) {
            if (depth < MAX_DEPTH && menu_count < MAX_MENUS) {
                menu_count++;
                HMENU sub = build_menu(m->submenu, depth + 1, above | (flags & MF_GRAYED));
                AppendMenuW(menu, flags | MF_POPUP, (UINT_PTR)sub, text);
            }
        } else if (item_count < MAX_ITEMS) {
            items[item_count] = m;
            built[item_count].cb = m->cb;
            built[item_count].flags = flags | above;
            AppendMenuW(menu, flags, ID_FIRST + item_count++, text);
        }
    }
    return menu;
}

// item_count goes with the menu, so items[] never describes a menu that is gone.
static void free_menu(void)
{
    if (hmenu)
        DestroyMenu(hmenu);          // destroys the submenus too
    hmenu = NULL;
    item_count = 0;
}

// Returns the exe or DLL this file is compiled into. GetModuleHandleW(NULL)
// always gives the exe.
static HINSTANCE this_module(void)
{
    static HMODULE m;
    if (!m) {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&g_tray, &m);
        if (!m)                      // e.g. a DLL loaded from memory
            m = GetModuleHandleW(NULL);
    }
    return m;
}

static int slash(wchar_t c) { return c == '\\' || c == '/'; }

// Puts the full path of p in out (MAX_PATH units), or returns 0 if p isn't the
// full path of a file: "C:\..." or "C:/...", or a network path
// ("\\server\share\..."). LoadImageW looks for any other path in the program's
// folder, the current folder and every folder on the PATH, where another
// program may have put a file of that name. The resolved path is checked, since
// a file named NUL in any folder comes out as "\\.\NUL": "\\.\" and "\\?\"
// paths open devices, and "\\server\pipe\..." a named pipe.
static int full_path(const wchar_t *p, wchar_t *out)
{
    int drive = (unsigned)((p[0] | 0x20) - 'a') < 26 && p[1] == ':' && slash(p[2]);
    if (!drive && !(slash(p[0]) && slash(p[1])))
        return 0;
    DWORD n = GetFullPathNameW(p, MAX_PATH, out, NULL);
    if (!n || n >= MAX_PATH)
        return 0;
    if (out[0] != '\\')
        return 1;
    const wchar_t *s = out + 2;
    if ((s[0] == '.' || s[0] == '?') && s[1] == '\\')
        return 0;
    while (*s && *s != '\\')         // past the server name
        s++;
    for (int k = 0; k < 4; k++)      // a share named pipe, in any case
        if (!s[0] || (s[k + 1] | 0x20) != "pipe"[k])
            return 1;
    return s[5] && s[5] != '\\';
}

// The icon from icon_id or icon_filepath, or NULL.
static HICON load_icon(const struct wintray *t)
{
    int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
    HICON h = NULL;
    if (t->icon_id) {
        // A resource ID has 16 bits; MAKEINTRESOURCE would cut a larger one
        // down to another icon's ID.
        if ((unsigned)t->icon_id > 0xFFFF)
            return NULL;
        // Look in the DLL wintray is built into first (if any), then in the exe.
        h = (HICON)LoadImageW(this_module(), MAKEINTRESOURCEW(t->icon_id),
                              IMAGE_ICON, cx, cy, 0);
        if (!h)
            h = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(t->icon_id),
                                  IMAGE_ICON, cx, cy, 0);
    } else if (t->icon_filepath) {
        // The whole path or none: a path cut short, or with invalid bytes
        // replaced, could name another file. MultiByteToWideChar fails for both.
        const char *s = t->icon_filepath;
        wchar_t path[MAX_PATH], full[MAX_PATH];
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, path, MAX_PATH) &&
            full_path(path, full))
            h = (HICON)LoadImageW(NULL, full, IMAGE_ICON, cx, cy, LR_LOADFROMFILE);
    }
    return h;
}

static BOOL notify(DWORD message)
{
    notifying++;
    BOOL ok = Shell_NotifyIconW(message, &nid);
    notifying--;
    return ok;
}

// NIM_MODIFY fails when Explorer doesn't know the icon (first call, or after
// an Explorer restart), so fall back to NIM_ADD. If that fails too, Explorer
// is probably busy (e.g. right after login): retry every 2 seconds.
// The window can close while Shell_NotifyIconW waits, so check hwnd after
// each call: no icon for a closed window, and no SetTimer(NULL, ...). An update
// made meanwhile calls show_icon() again with newer data, and that call decides
// about NIM_ADD and the timer.
static void show_icon(void)
{
    static unsigned calls;
    unsigned me = ++calls;
    int ok = notify(NIM_MODIFY);
    if (!ok && hwnd && calls == me) {
        ok = notify(NIM_ADD);
        // Closed meanwhile: Explorer may still add the icon after cleanup()'s
        // NIM_DELETE, even when NIM_ADD reported a timeout.
        if (!hwnd)
            notify(NIM_DELETE);
    }
    if (!hwnd || calls != me)
        return;
    if (ok)
        KillTimer(hwnd, RETRY_TIMER);
    else
        SetTimer(hwnd, RETRY_TIMER, 2000, NULL);
}

// An update came while the menu was open, so the clicked item may have changed
// or left the menu, and a program may free an item it took out. So run the
// update first, and keep the click on item i only if its command still shows
// the same text, state and callback. Both texts are read from the menus, never
// from the item.
static int update_and_check(UINT i)
{
    if (i >= item_count) {           // nothing chosen
        wintray_update(g_tray);
        return 0;
    }
    void (*cb)(struct wintray_menu_item *) = built[i].cb;
    UINT flags = built[i].flags;
    wchar_t was[MAX_TEXT], now[MAX_TEXT];
    was[0] = now[0] = 0;
    GetMenuStringW(hmenu, ID_FIRST + i, was, ARRAYSIZE(was), MF_BYCOMMAND);
    wintray_update(g_tray);
    // A window closed meanwhile has freed its menu, so item_count is 0, and no
    // new one can be made while the update waits for Explorer.
    if (i >= item_count || built[i].cb != cb || built[i].flags != flags)
        return 0;
    GetMenuStringW(hmenu, ID_FIRST + i, now, ARRAYSIZE(now), MF_BYCOMMAND);
    for (int k = 0; was[k] == now[k]; k++)
        if (!was[k])
            return 1;
    return 0;
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
    if (!hwnd) {                     // closed while the menu was open
        free_menu();
        return;
    }
    PostMessageW(hwnd, WM_NULL, 0, 0);
    UINT i = cmd - ID_FIRST;         // cmd 0 (nothing chosen) wraps around
    if (update_pending && !update_and_check(i))
        return;
    if (i < item_count && items[i]->cb)
        items[i]->cb(items[i]);
}

// Runs on WM_DESTROY, so it also cleans up when the window is closed by
// someone else (taskkill without /f, an installer, ...).
static void cleanup(void)
{
    // Clear hwnd first, so wnd_proc() ignores clicks that come in while
    // Shell_NotifyIconW waits for Explorer.
    hwnd = NULL;
    closing = 1;
    notify(NIM_DELETE);
    update_pending = 0;
    if (menu_open)
        EndMenu();                   // show_menu() frees the menu when it returns
    else
        free_menu();
    if (own_icon)
        DestroyIcon(own_icon);
    own_icon = nid.hIcon = NULL;
    closing = 0;
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    // A WM_CLOSE while cleanup() runs would destroy the window a second time.
    if (msg == WM_CLOSE && closing)
        return 0;
    // Windows lets the taskbar's close, minimize and restore commands through
    // from programs of lower integrity, and after the menu the window has the
    // keyboard focus, where Wine turns Alt+F4 into SC_CLOSE. The hidden window
    // takes none of them.
    if (msg == WM_SYSCOMMAND) {
        UINT sc = (UINT)wp & 0xFFF0;
        if (sc == SC_CLOSE || sc == SC_MINIMIZE || sc == SC_MAXIMIZE || sc == SC_RESTORE)
            return 0;
    }
    // Still in CreateWindowW, closing, or after a WM_DESTROY from another program.
    if (!hwnd)
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
    // The retry timer from show_icon(), or TaskbarCreated: Explorer restarted.
    if ((msg == WM_TIMER && wp == RETRY_TIMER) || (msg == wm_taskbar_created && msg != 0)) {
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

// Whether w is still the window wintray made. Another program can send it
// WM_DESTROY and WM_NCDESTROY and leave it standing, and the program may destroy
// it itself, so window is checked where it is used. Its class's window
// procedure, which subclassing doesn't change, tells it from another window
// that got the same handle later.
static int still_there(HWND w)
{
    return w && IsWindow(w) && (WNDPROC)GetClassLongPtrW(w, GCLP_WNDPROC) == wnd_proc;
}

struct wintray *wintray_get_instance(void) { return g_tray; }

int wintray_init(struct wintray *tray)
{
    // Not while the menu is open: its window may be closing, and the new one
    // would get no menu until it closes. Nor while Shell_NotifyIconW waits:
    // Explorer may still add the icon for a closed window, and show_icon()
    // removes it only while there is no new window.
    if (!tray || hwnd || closing || menu_open || notifying)
        return -1;
    // Left by a WM_DESTROY from another program. Kept if it can't be destroyed
    // (another thread), so wintray_exit() can try again.
    if (still_there(window) && !DestroyWindow(window))
        return -1;
    window = NULL;
    wm_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    // Hidden window for the icon messages. Not a message-only window, because
    // those don't receive broadcasts such as TaskbarCreated.
    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = this_module();
    wc.lpszClassName = CLASS_NAME;
    // Remove any old registration first. One is left over if the window was
    // closed from outside, and after a DLL reload it points to a wnd_proc
    // that no longer exists.
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (!RegisterClassW(&wc))
        return -1;
    hwnd = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);   // or it may outlive a DLL
        return -1;
    }
    window = hwnd;
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
    update_pending = 0;              // this update covers a pending one

    free_menu();
    menu_count = 0;
    hmenu = build_menu(tray->menu, 0, 0);

    HICON old = own_icon;
    own_icon = load_icon(tray);
    // No icon of our own: IDI_APPLICATION (32512), a shared icon, never destroyed.
    nid.hIcon = own_icon ? own_icon : LoadIconW(NULL, MAKEINTRESOURCEW(32512));
    to_wide(tray->tooltip, nid.szTip, ARRAYSIZE(nid.szTip));
    show_icon();
    if (old)
        DestroyIcon(old);
}

void wintray_exit(void)
{
    // window, not hwnd: after a WM_DESTROY from another program, the window is
    // still there. Not while cleanup() runs: that is usually inside DestroyWindow.
    if (closing)
        return;
    if (still_there(window)) {
        if (!DestroyWindow(window))  // the rest happens in WM_DESTROY
            return;                  // another thread: keep it for another try
        // Windows doesn't unregister a DLL's classes when the DLL is unloaded.
        UnregisterClassW(CLASS_NAME, this_module());
    }
    window = NULL;
}
