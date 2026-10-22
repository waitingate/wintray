// Logic tests for wintray.c, run on Linux with fake Win32 functions that
// model Explorer, menus, windows, classes and icons. Run with: sh test/run.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../wintray.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// Runs a hook once. It is cleared before the call, since the hook may lead back here.
static void run_hook(void (**h)(void))
{
    void (*f)(void) = *h;
    *h = NULL;
    if (f)
        f();
}

/* ---------------- modules ---------------- */
#define EXE ((HINSTANCE)(uintptr_t)0x400000)
#define DLL ((HINSTANCE)(uintptr_t)0x10000000)
static int mode_dll, mode_gmh_fail, gmh_calls;
HMODULE GetModuleHandleW(LPCWSTR n) { (void)n; return EXE; }
BOOL GetModuleHandleExW(DWORD f, LPCWSTR a, HMODULE *m)
{
    (void)f; (void)a;
    gmh_calls++;
    *m = mode_gmh_fail ? NULL : mode_dll ? DLL : EXE;
    return *m != NULL;
}

/* ---------------- strings ---------------- */
int lstrlenA(LPCSTR s) { return s ? (int)strlen(s) : 0; }

// Strict UTF-8 decoder. An invalid byte becomes U+FFFD and uses one byte.
static int dec(const unsigned char *s, int n, int i, unsigned *cp)
{
    unsigned c = s[i], min;
    int k;
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0)      { k = 2; min = 0x80;    c &= 0x1F; }
    else if ((c & 0xF0) == 0xE0) { k = 3; min = 0x800;   c &= 0x0F; }
    else if ((c & 0xF8) == 0xF0) { k = 4; min = 0x10000; c &= 0x07; }
    else { *cp = 0xFFFD; return 1; }
    if (i + k > n) { *cp = 0xFFFD; return 1; }
    for (int j = 1; j < k; j++) {
        if ((s[i + j] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        c = c << 6 | (s[i + j] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) { *cp = 0xFFFD; return 1; }
    *cp = c;
    return k;
}
static void put_utf16(unsigned c, wchar_t *p)
{
    if (c < 0x10000) { p[0] = (wchar_t)c; return; }
    p[0] = (wchar_t)(0xD800 + ((c - 0x10000) >> 10));
    p[1] = (wchar_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
}

// Converts whole characters while they fit in max units; out can be NULL.
// Returns the units, and the bytes converted in *used.
static int utf8_to_16(const char *src, int n, wchar_t *out, int max, int *used)
{
    const unsigned char *s = (const unsigned char *)src;
    int units = 0, i = 0;
    while (i < n) {
        unsigned c;
        int k = dec(s, n, i, &c), u = c >= 0x10000 ? 2 : 1;
        if (units + u > max)
            break;
        if (out)
            put_utf16(c, out + units);
        units += u;
        i += k;
    }
    *used = i;
    return units;
}

static long mb_calls;
int MultiByteToWideChar(UINT cp, DWORD fl, LPCSTR src, int n, wchar_t *out, int outn)
{
    (void)cp; (void)fl;
    mb_calls++;
    // Without out, n is enough: no character gives more units than bytes.
    int used, units = utf8_to_16(src, n, out, out ? outn : n, &used);
    return used < n ? 0 : units;            // 0 when the buffer is too small
}

/* ---------------- menus ---------------- */
#define MAXM 200000
static int m_live[MAXM], m_parent[MAXM];
static int menus_created, menus_live, on_screen;
static int n_popups, n_leaves, last_leaf_id;
HMENU CreatePopupMenu(void)
{
    int id = ++menus_created;
    m_live[id] = 1;
    menus_live++;
    return (HMENU)(uintptr_t)id;
}
BOOL AppendMenuW(HMENU m, UINT f, UINT_PTR id, LPCWSTR t)
{
    (void)t;
    int mi = (int)(uintptr_t)m;
    CHECK(m_live[mi]);
    if (f & MF_POPUP) {
        m_parent[(int)id] = mi;
        n_popups++;
    } else if (!(f & MF_SEPARATOR)) {
        n_leaves++;
        last_leaf_id = (int)id;
    }
    return TRUE;
}
static int below(int i, int anc)
{
    for (int p = m_parent[i]; p; p = m_parent[p])
        if (p == anc)
            return 1;
    return 0;
}
BOOL DestroyMenu(HMENU m)
{
    int mi = (int)(uintptr_t)m;
    if (!mi)
        return FALSE;
    CHECK(m_live[mi]);                      // not destroyed twice
    CHECK(mi != on_screen);                 // not while it's on screen
    m_live[mi] = 0;
    menus_live--;
    for (int i = mi + 1; i <= menus_created; i++)
        if (m_live[i] && below(i, mi)) {
            m_live[i] = 0;
            menus_live--;
        }
    return TRUE;
}
static void (*track_hook)(void);
static int track_calls, track_ret, end_menu_called;
BOOL TrackPopupMenu(HMENU m, UINT f, int x, int y, int r, HWND h, const void *rc)
{
    (void)f; (void)x; (void)y; (void)r; (void)rc;
    track_calls++;
    CHECK(!on_screen);                      // never two menus at once
    CHECK(h != NULL && m != NULL);
    on_screen = (int)(uintptr_t)m;
    end_menu_called = 0;
    run_hook(&track_hook);                  // things that happen while the menu is open
    on_screen = 0;
    return end_menu_called ? 0 : track_ret;
}
BOOL EndMenu(void) { end_menu_called = 1; return TRUE; }

/* ---------------- icons ---------------- */
#define SHARED ((HICON)(uintptr_t)0xABCDE0)
static int i_live[MAXM], icons_created, icons_live;
static int icon_in_dll, icon_in_exe = 1, from_dll, from_exe;
HANDLE LoadImageW(HINSTANCE h, LPCWSTR name, UINT type, int cx, int cy, UINT fl)
{
    (void)type; (void)cx; (void)cy;
    int ok = 0;
    if (fl & LR_LOADFROMFILE) {
        ok = name[0] == 'g';                // "good.ico" exists
    } else if (h == DLL) {
        ok = icon_in_dll;
        from_dll += ok;
    } else if (h == EXE) {
        ok = icon_in_exe;
        from_exe += ok;
    }
    if (!ok)
        return NULL;
    int id = ++icons_created;
    i_live[id] = 1;
    icons_live++;
    return (HANDLE)(uintptr_t)id;
}
HICON LoadIconW(HINSTANCE h, LPCWSTR n) { (void)h; (void)n; return SHARED; }
BOOL DestroyIcon(HICON i)
{
    CHECK(i != SHARED);                     // the shared icon is never destroyed
    if (i == SHARED)
        return FALSE;
    int id = (int)(uintptr_t)i;
    CHECK(i_live[id]);                      // not destroyed twice
    if (!i_live[id])
        return FALSE;
    i_live[id] = 0;
    icons_live--;
    return TRUE;
}
int GetSystemMetrics(int i) { (void)i; return 16; }

/* ---------------- classes and windows ---------------- */
struct cls { int used; LPCWSTR name; HINSTANCE inst; WNDPROC proc; int windows; };
static struct cls classes[8];
static int wsame(LPCWSTR a, LPCWSTR b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static struct cls *find_class(LPCWSTR name, HINSTANCE inst)
{
    for (int i = 0; i < 8; i++)
        if (classes[i].used && classes[i].inst == inst && wsame(classes[i].name, name))
            return &classes[i];
    return NULL;
}
unsigned short RegisterClassW(const WNDCLASSW *wc)
{
    if (find_class(wc->lpszClassName, wc->hInstance))
        return 0;                           // ERROR_CLASS_ALREADY_EXISTS
    for (int i = 0; i < 8; i++)
        if (!classes[i].used) {
            classes[i] = (struct cls){1, wc->lpszClassName, wc->hInstance, wc->lpfnWndProc, 0};
            return (unsigned short)(i + 1);
        }
    return 0;
}
BOOL UnregisterClassW(LPCWSTR name, HINSTANCE inst)
{
    struct cls *c = find_class(name, inst);
    if (!c || c->windows)
        return FALSE;
    c->used = 0;
    return TRUE;
}
struct win { int state; struct cls *c; };  // 1 alive, 2 being destroyed, 0 gone
static struct win wins[1000];
static int nwins;
#define WIN(h) wins[(uintptr_t)(h) / 16]
HWND CreateWindowW(LPCWSTR cls, LPCWSTR title, DWORD st, int x, int y, int w, int h,
                   HWND parent, HMENU menu, HINSTANCE inst, void *param)
{
    (void)title; (void)st; (void)x; (void)y; (void)w; (void)h; (void)parent; (void)menu; (void)param;
    struct cls *c = find_class(cls, inst);
    if (!c)
        return NULL;
    HWND hw = (HWND)(uintptr_t)(++nwins * 16);
    WIN(hw) = (struct win){1, c};
    c->windows++;
    c->proc(hw, 0x0081, 0, 0);              // WM_NCCREATE
    c->proc(hw, 0x0001, 0, 0);              // WM_CREATE
    return hw;
}
BOOL DestroyWindow(HWND hw)
{
    if (WIN(hw).state != 1)
        return FALSE;
    WIN(hw).state = 2;
    WIN(hw).c->proc(hw, WM_DESTROY, 0, 0);
    WIN(hw).c->proc(hw, 0x0082, 0, 0);      // WM_NCDESTROY
    WIN(hw).state = 0;
    WIN(hw).c->windows--;
    return TRUE;
}

/* ---------------- Explorer ---------------- */
static int explorer_up = 1, icon_present, ghosts;
static void (*notify_hook)(DWORD);
BOOL Shell_NotifyIconW(DWORD m, NOTIFYICONDATAW *d)
{
    if (notify_hook)
        notify_hook(m);                     // messages handled while it waits for Explorer
    if (m == NIM_DELETE) {
        icon_present = 0;
        return TRUE;
    }
    if (WIN(d->hWnd).state != 1)
        ghosts++;                           // icon added or changed for a closing window
    if (!explorer_up)
        return FALSE;
    if (m == NIM_MODIFY)
        return icon_present;
    icon_present = 1;
    return TRUE;
}

/* ---------------- the rest ---------------- */
static int retry_timer_on, thread_timers, quit_posted;
UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT ms, void *f)
{
    (void)ms; (void)f;
    if (!h) {
        thread_timers++;
        return 99;
    }
    retry_timer_on = 1;
    return id;
}
BOOL KillTimer(HWND h, UINT_PTR id) { (void)id; if (h) retry_timer_on = 0; return TRUE; }
BOOL GetCursorPos(POINT *p) { p->x = p->y = 0; return TRUE; }
BOOL SetForegroundWindow(HWND h) { (void)h; return TRUE; }
BOOL PostMessageW(HWND h, UINT m, WPARAM w, LPARAM l) { (void)h; (void)m; (void)w; (void)l; return TRUE; }
void PostQuitMessage(int c) { (void)c; quit_posted++; }
LRESULT DefWindowProcW(HWND h, UINT m, WPARAM w, LPARAM l) { (void)h; (void)m; (void)w; (void)l; return 0; }
#define TASKBAR_CREATED 0xC100
UINT RegisterWindowMessageW(LPCWSTR n) { return wsame(n, L"TaskbarCreated") ? TASKBAR_CREATED : 0xC101; }
BOOL ChangeWindowMessageFilterEx(HWND h, UINT m, DWORD a, void *p) { (void)h; (void)m; (void)a; (void)p; return TRUE; }
static int get_calls;
BOOL GetMessageW(MSG *m, HWND h, UINT a, UINT b)
{
    (void)h; (void)a; (void)b;
    get_calls++;
    if (quit_posted) { quit_posted--; m->message = WM_QUIT; return 0; }
    m->message = WM_NULL;
    m->hwnd = NULL;
    return 1;
}
BOOL PeekMessageW(MSG *m, HWND h, UINT a, UINT b, UINT r)
{
    (void)h; (void)a; (void)b; (void)r;
    if (quit_posted) { quit_posted--; m->message = WM_QUIT; return 1; }
    return 0;
}
BOOL TranslateMessage(const MSG *m) { (void)m; return TRUE; }
LRESULT DispatchMessageW(const MSG *m) { (void)m; return 0; }

/* ---------------- to_wide ---------------- */
// The same cuts as to_wide, but it steps back one byte at a time instead of one
// per unit too many. The reference for invalid UTF-8 and for the call count.
static void to_wide_bytewise(const char *s, wchar_t *out, int size)
{
    out[0] = 0;
    if (!s)
        return;
    int bytes = lstrlenA(s);
    if (bytes > 3 * (size - 1))
        bytes = 3 * (size - 1);
    for (;;) {
        int cut = bytes;
        while (bytes > 0 && cut - bytes < 3 && (s[bytes] & 0xC0) == 0x80)
            bytes--;
        if ((s[bytes] & 0xC0) == 0x80)
            bytes = cut;
        int need = bytes ? MultiByteToWideChar(CP_UTF8, 0, s, bytes, NULL, 0) : 0;
        if (need <= size - 1) {
            int n = need ? MultiByteToWideChar(CP_UTF8, 0, s, bytes, out, size - 1) : 0;
            out[n] = 0;
            return;
        }
        bytes--;
    }
}
// Expected result for valid UTF-8: the longest run of whole characters that fits.
static void ref_conv(const char *s, int size, wchar_t *out)
{
    int used;
    out[utf8_to_16(s, (int)strlen(s), out, size - 1, &used)] = 0;
}
static int put_utf8(unsigned c, char *p)
{
    if (c < 0x80) { p[0] = (char)c; return 1; }
    if (c < 0x800) { p[0] = (char)(0xC0 | c >> 6); p[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        p[0] = (char)(0xE0 | c >> 12); p[1] = (char)(0x80 | (c >> 6 & 0x3F)); p[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    p[0] = (char)(0xF0 | c >> 18); p[1] = (char)(0x80 | (c >> 12 & 0x3F));
    p[2] = (char)(0x80 | (c >> 6 & 0x3F)); p[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}
static unsigned rand_cp(void)
{
    unsigned c;
    switch (rand() % 4) {
    case 0: return 1 + rand() % 0x7F;
    case 1: return 0x80 + rand() % (0x800 - 0x80);
    case 2: do c = 0x800 + rand() % (0x10000 - 0x800); while (c >= 0xD800 && c <= 0xDFFF); return c;
    default: return 0x10000 + rand() % (0x110000 - 0x10000);
    }
}

static void test_to_wide(void)
{
    printf("to_wide\n");
    static char s[1400];
    static wchar_t a[300], b[300], r[300];
    static const int small[] = {1, 2, 3, 4, 5, 6, 8, 13, 32}, big[] = {128, 256, 260};
    int bad_ref = 0, bad_inv = 0, overlong = 0;
    srand(1);
    for (int it = 0; it < 101000; it++) {           // valid UTF-8, all character sizes
        int bigcase = it >= 100000;
        int size = bigcase ? big[rand() % 3] : small[rand() % 9];
        int target = rand() % ((bigcase ? 1200 : 120) + 1), len = 0;
        while (len < target)
            len += put_utf8(rand_cp(), s + len);
        s[len] = 0;
        to_wide(s, a, size);
        ref_conv(s, size, r);
        bad_ref += !wsame(a, r);
    }
    for (int it = 0; it < 50000; it++) {            // random bytes, mostly not valid UTF-8
        int size = small[rand() % 9], len = rand() % 150;
        for (int i = 0; i < len; i++)
            s[i] = (char)(1 + rand() % 255);
        s[len] = 0;
        to_wide(s, a, size);
        to_wide_bytewise(s, b, size);
        bad_inv += !wsame(a, b);
        int n = 0;
        while (a[n])
            n++;
        overlong += n > size - 1;
    }
    CHECK(bad_ref == 0);
    CHECK(bad_inv == 0);
    CHECK(overlong == 0);
    memset(s, 'a', 1000);
    s[1000] = 0;
    mb_calls = 0; to_wide(s, a, 256); long now = mb_calls;
    mb_calls = 0; to_wide_bytewise(s, b, 256); long bytewise = mb_calls;
    printf("  1000 chars cut to 255: %ld conversion calls, %ld one byte at a time\n", now, bytewise);
    CHECK(now <= 2);
    mb_calls = 0; to_wide("Exit", a, 256);
    CHECK(mb_calls == 1);                           // shorter than the buffer: no size query

    memset(s, 'x', 3);                              // invalid: "xxx" and 200 stray continuation bytes
    memset(s + 3, 0x80, 200);
    s[203] = 0;
    to_wide(s, a, 128);
    int units = 0;
    while (a[units])
        units++;
    CHECK(units == 127);                            // the cut stays among the stray bytes, each one U+FFFD
}

/* ---------------- tray tests ---------------- */
static int cb_calls, item_calls;
static void (*cb_hook)(void), (*item_hook)(void);
static void tray_cb(struct wintray *t)
{
    (void)t;
    cb_calls++;
    run_hook(&cb_hook);
}
static void item_cb(struct wintray_menu_item *m)
{
    (void)m;
    item_calls++;
    run_hook(&item_hook);
}
static struct wintray_menu_item menu1[] = {
    {"Item", 0, 0, item_cb, NULL}, {"-", 0, 0, NULL, NULL}, {"Exit", 0, 0, item_cb, NULL},
    {NULL, 0, 0, NULL, NULL}};
static struct wintray t_cb = {NULL, "tip", tray_cb, menu1, 1};
static struct wintray t_menu = {NULL, "tip", NULL, menu1, 1};

static HWND W;
static void start(struct wintray *t) { CHECK(wintray_init(t) == 0); W = hwnd; }
// After wintray_exit() or the window being destroyed: everything is gone.
// closed() and stop() are macros, so a failure also names the test's line.
static void closed_at(int line, int blocking)
{
    int before = fails;
    CHECK(quit_posted == 1);
    CHECK(wintray_loop(blocking) == -1 && quit_posted == 0);   // the loop takes the WM_QUIT
    CHECK(hwnd == NULL && menus_live == 0 && icons_live == 0 && !icon_present && !update_pending);
    CHECK(!track_hook && !cb_hook && !item_hook && !notify_hook);   // every hook ran
    quit_posted = 0;                                // a failure here must not fail later tests too
    track_hook = cb_hook = item_hook = NULL;
    notify_hook = NULL;
    if (fails > before)
        printf("    called from line %d\n", line);
}
#define closed() closed_at(__LINE__, 0)
#define stop() (wintray_exit(), closed())
static void ev(UINT e) { wnd_proc(W, WM_TRAY, 1, e); }
static void left_click(void) { ev(WM_LBUTTONDOWN); ev(WM_LBUTTONUP); }
static void second_click(void) { ev(WM_LBUTTONDBLCLK); ev(WM_LBUTTONUP); }
static void right_click(void) { ev(WM_RBUTTONDOWN); ev(WM_RBUTTONUP); }

static void test_clicks(void)
{
    printf("clicks\n");
    start(&t_cb);
    cb_calls = 0; left_click();
    CHECK(cb_calls == 1);                           // single click
    cb_calls = 0; left_click(); second_click();
    CHECK(cb_calls == 1);                           // double-click
    cb_calls = 0; left_click(); second_click(); left_click();
    CHECK(cb_calls == 2);                           // triple click
    cb_calls = 0; left_click(); ev(WM_LBUTTONDBLCLK); left_click();
    CHECK(cb_calls == 2);                           // a DBLCLK without its UP doesn't eat the next click
    cb_calls = 0; cb_hook = second_click; left_click();
    CHECK(cb_calls == 1);                           // 2nd click handled while cb shows a dialog
    cb_calls = 0; track_calls = 0; right_click();
    CHECK(cb_calls == 0 && track_calls == 1);       // right click opens the menu
    stop();

    start(&t_menu);
    track_calls = 0; left_click();
    CHECK(track_calls == 1);                        // no cb: left click opens the menu
    track_calls = 0; left_click(); second_click();
    CHECK(track_calls == 1);                        // double-click opens it once
    track_calls = 0; track_hook = second_click; left_click();
    CHECK(track_calls == 1);                        // 2nd click arrives while the menu is open
    stop();
}

static void hook_update(void)
{
    int before = menus_created;
    wintray_update(g_tray);
    CHECK(menus_created == before && update_pending == 1);  // waits for the menu to close
}
static void hook_exit(void) { wintray_exit(); CHECK(end_menu_called && hwnd == NULL); }
static void do_update(void) { wintray_update(g_tray); }

static void test_menu(void)
{
    printf("menu\n");
    start(&t_menu);
    track_calls = 0; track_hook = right_click; right_click();
    CHECK(track_calls == 1 && menu_open == 0);      // clicked again while the menu is open
    int before = menus_created;
    track_hook = hook_update; right_click();
    CHECK(menus_created > before && update_pending == 0);   // rebuilt once it closed
    before = menus_created;
    track_hook = hook_update; track_ret = ID_FIRST; item_hook = do_update; item_calls = 0;
    right_click();
    CHECK(item_calls == 1 && update_pending == 0);  // item cb updates, plus a pending update
    CHECK(menus_created == before + 1);             // rebuilt once, not twice
    track_ret = 0;
    track_hook = hook_exit; right_click();          // wintray_exit() while the menu is open
    closed();
    CHECK(hmenu == NULL);

    start(&t_menu);                                 // the "Exit" item, with an update waiting
    track_hook = hook_update; track_ret = ID_FIRST + 1; item_hook = wintray_exit; item_calls = 0;
    right_click();
    CHECK(item_calls == 1);
    closed();
    track_ret = 0;
}

static void on_delete(DWORD m)
{
    if (m != NIM_DELETE)
        return;
    notify_hook = NULL;
    left_click(); right_click();
    wnd_proc(W, TASKBAR_CREATED, 0, 0);
}
static void test_closing(void)
{
    printf("closing\n");
    start(&t_cb);
    cb_calls = track_calls = ghosts = 0;
    notify_hook = on_delete;                        // clicks while the icon is being removed
    stop();
    CHECK(cb_calls == 0 && track_calls == 0 && ghosts == 0);

    start(&t_menu);                                 // closed from outside
    DestroyWindow(W);
    closed();
    CHECK(find_class(CLASS_NAME, this_module()) != NULL);   // class still registered
    start(&t_menu);                                 // init works again
    stop();
    CHECK(find_class(CLASS_NAME, this_module()) == NULL);   // exit unregisters it

    WNDCLASSW old = {0};                            // left over from an earlier DLL load
    old.lpfnWndProc = DefWindowProcW;
    old.hInstance = this_module();
    old.lpszClassName = CLASS_NAME;
    CHECK(RegisterClassW(&old) != 0);
    start(&t_menu);
    struct cls *c = find_class(CLASS_NAME, old.hInstance);
    CHECK(c && c->proc == wnd_proc);
    CHECK(wintray_init(&t_menu) == -1);             // already running
    stop();
    CHECK(wintray_init(NULL) == -1);
}

static void test_icons(void)
{
    printf("icons\n");
    static struct wintray t = {NULL, "tip", NULL, menu1, 1};
    icon_in_dll = 0; icon_in_exe = 1; from_dll = from_exe = 0;
    start(&t);
    CHECK(icon_owned && from_exe == 1 && from_dll == 0);   // icon in the exe
    if (mode_dll) {
        icon_in_dll = 1;
        wintray_update(&t);
        CHECK(icon_owned && from_dll == 1 && from_exe == 1);   // the DLL's own icon comes first
    }
    icon_in_dll = icon_in_exe = 0;
    wintray_update(&t);
    CHECK(nid.hIcon == SHARED && !icon_owned);     // no icon anywhere: shared default
    wintray_update(&t);                             // and it's never destroyed
    t.icon_id = 0; t.icon_filepath = "good.ico"; wintray_update(&t);
    CHECK(icon_owned);
    t.icon_filepath = "bad.ico"; wintray_update(&t);
    CHECK(nid.hIcon == SHARED);
    t.icon_id = 1; icon_in_exe = 1;
    for (int i = 0; i < 100; i++)
        wintray_update(&t);
    CHECK(icons_live == 1);                         // no leaks
    stop();
    CHECK(gmh_calls == 1);                          // module looked up once
    CHECK(this_module() == (mode_dll ? DLL : EXE));
}

static DWORD close_on;                              // the Shell_NotifyIconW call to close during
static void close_during(DWORD m)
{
    if (m != close_on)
        return;
    notify_hook = NULL;
    DestroyWindow(W);
}
static void test_explorer(void)
{
    printf("explorer\n");
    explorer_up = 0;
    start(&t_menu);
    CHECK(retry_timer_on && !icon_present);         // Explorer not running yet
    explorer_up = 1;
    wnd_proc(W, WM_TIMER, RETRY_TIMER, 0);
    CHECK(icon_present && !retry_timer_on);         // added on retry
    icon_present = 0;
    wnd_proc(W, TASKBAR_CREATED, 0, 0);
    CHECK(icon_present);                            // re-added after an Explorer restart
    stop();

    // Closed while Shell_NotifyIconW waits for Explorer: no icon may be left for the
    // closed window, and no SetTimer(NULL, ...). Without an icon, NIM_MODIFY fails
    // and NIM_ADD comes next; with Explorer down, NIM_ADD fails too.
    static const struct { DWORD call; int icon, up; } cases[] = {
        {NIM_MODIFY, 1, 1}, {NIM_ADD, 0, 1}, {NIM_ADD, 0, 0}};
    for (int i = 0; i < 3; i++) {
        int before = fails;
        start(&t_menu);
        icon_present = cases[i].icon;
        explorer_up = cases[i].up;
        close_on = cases[i].call;
        notify_hook = close_during;
        wnd_proc(W, TASKBAR_CREATED, 0, 0);
        explorer_up = 1;
        closed();
        CHECK(thread_timers == 0);
        if (fails > before)
            printf("    in case %d\n", i);
    }
}

static void test_loop(void)
{
    printf("loop\n");
    start(&t_menu);
    get_calls = 0;
    CHECK(wintray_loop(0) == 0 && get_calls == 0);  // nothing waiting, and it doesn't wait
    CHECK(wintray_loop(1) == 0 && get_calls == 1);  // waits in GetMessageW for a message
    wintray_exit();
    closed_at(__LINE__, 1);                         // the blocking loop sees WM_QUIT too
}

static struct wintray_menu_item selfm[5], top[71], subs[70][4], many[301];
static struct wintray t_self = {NULL, "t", NULL, selfm, 0};
static struct wintray t_top = {NULL, "t", NULL, top, 0};
static struct wintray t_many = {NULL, "t", NULL, many, 0};
static void test_limits(void)
{
    printf("menu limits\n");
    for (int i = 0; i < 4; i++)
        selfm[i] = (struct wintray_menu_item){"S", 0, 0, NULL, selfm};
    int before = menus_created;
    n_popups = 0;
    start(&t_self);                                 // a menu that contains itself
    int deepest = 0;                                // depth = number of parents, once built
    for (int i = before + 1; i <= menus_created; i++) {
        int d = 0;
        for (int p = m_parent[i]; p; p = m_parent[p])
            d++;
        if (d > deepest)
            deepest = d;
    }
    printf("  menu that contains itself: %d submenus, %d levels\n", n_popups, deepest);
    CHECK(menus_created - before == 1 + MAX_MENUS);
    CHECK(n_popups == MAX_MENUS);
    CHECK(deepest == MAX_DEPTH);
    stop();

    for (int i = 0; i < 70; i++) {
        for (int j = 0; j < 3; j++)
            subs[i][j] = (struct wintray_menu_item){"x", 0, 0, item_cb, NULL};
        top[i] = (struct wintray_menu_item){"P", 0, 0, NULL, subs[i]};
    }
    n_popups = n_leaves = 0;
    start(&t_top);
    CHECK(n_popups == 70 && n_leaves == 210);       // a big normal menu is complete
    stop();

    for (int i = 0; i < 300; i++)
        many[i] = (struct wintray_menu_item){"m", 0, 0, item_cb, NULL};
    n_leaves = 0;
    start(&t_many);
    CHECK(n_leaves == MAX_ITEMS && last_leaf_id == ID_FIRST + MAX_ITEMS - 1);
    stop();
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "exe";
    mode_dll = !strcmp(mode, "dll");
    mode_gmh_fail = !strcmp(mode, "gmhfail");
    printf("== %s\n", mode);
    if (!strcmp(mode, "exe"))
        test_to_wide();
    test_clicks();
    test_menu();
    test_closing();
    test_icons();
    test_explorer();
    test_loop();
    test_limits();
    CHECK(thread_timers == 0);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
