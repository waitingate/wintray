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
    (void)a;
    gmh_calls++;
    // Without FROM_ADDRESS, a would be read as a module name, and without
    // UNCHANGED_REFCOUNT the DLL could never be unloaded.
    int flags_ok = f == (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT);
    *m = mode_gmh_fail || !flags_ok ? NULL : mode_dll ? DLL : EXE;
    return *m != NULL;
}

/* ---------------- strings ---------------- */
int lstrlenA(LPCSTR s) { return s ? (int)strlen(s) : 0; }
static int wsame(LPCWSTR a, LPCWSTR b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static int wlen(LPCWSTR s) { int n = 0; while (s[n]) n++; return n; }

// Whether a valid code point of a k-byte sequence starts with the bits in v,
// which come from its first j bytes.
static int can_start(unsigned v, int j, int k, unsigned min)
{
    int shift = 6 * (k - j);
    unsigned lo = v << shift, hi = lo + (1u << shift) - 1;
    return hi >= min && lo <= 0x10FFFF && !(lo >= 0xD800 && hi <= 0xDFFF);
}

// Strict UTF-8 decoder that splits invalid text the way Windows does. A cut-off
// sequence becomes one U+FFFD for the longest start of it that a character could
// have (at least one byte), a lead byte followed by a continuation byte that no
// character can have becomes one for both, and a stray byte becomes one of its own.
static int dec(const unsigned char *s, int n, int i, unsigned *cp)
{
    unsigned c = s[i], min;
    int k;
    if (c < 0x80) { *cp = c; return 1; }
    *cp = 0xFFFD;
    if ((c & 0xE0) == 0xC0)      { k = 2; min = 0x80;    c &= 0x1F; }
    else if ((c & 0xF0) == 0xE0) { k = 3; min = 0x800;   c &= 0x0F; }
    else if ((c & 0xF8) == 0xF0) { k = 4; min = 0x10000; c &= 0x07; }
    else return 1;
    if (!can_start(c, 1, k, min))
        return 1;
    for (int j = 1; j < k; j++) {
        if (i + j >= n || (s[i + j] & 0xC0) != 0x80)
            return j;
        unsigned v = c << 6 | (s[i + j] & 0x3F);
        if (!can_start(v, j + 1, k, min))
            return j + 1;
        c = v;
    }
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

// Whether the text is valid UTF-8. The decoder gives U+FFFD for invalid bytes,
// and for valid text only as the three bytes of U+FFFD itself.
static int valid_utf8(const char *src, int n)
{
    const unsigned char *s = (const unsigned char *)src;
    for (int i = 0; i < n; ) {
        unsigned c;
        int k = dec(s, n, i, &c);
        if (c == 0xFFFD && !(k == 3 && s[i] == 0xEF))
            return 0;
        i += k;
    }
    return 1;
}

static long mb_calls;
int MultiByteToWideChar(UINT cp, DWORD fl, LPCSTR src, int n, wchar_t *out, int outn)
{
    (void)cp;
    mb_calls++;
    if (n == -1)
        n = (int)strlen(src) + 1;           // the terminator is converted too
    if ((fl & MB_ERR_INVALID_CHARS) && !valid_utf8(src, n))
        return 0;                           // ERROR_NO_UNICODE_TRANSLATION
    // Without out, n is enough: no character gives more units than bytes.
    int used, units = utf8_to_16(src, n, out, out ? outn : n, &used);
    return used < n ? 0 : units;            // 0 when the buffer is too small
}

/* ---------------- menus ---------------- */
#define MAXM 200000
static int m_live[MAXM], m_parent[MAXM];
static int menus_created, menus_live, on_screen;
static int n_popups, n_leaves, n_separators, last_leaf_id;
// What AppendMenuW got for each command ID, and the text length of each
// submenu's item (-1 for no text).
static UINT got_flags[ID_FIRST + MAX_ITEMS];
static int got_len[ID_FIRST + MAX_ITEMS], sub_len[MAXM];
static int got_menu[ID_FIRST + MAX_ITEMS];          // the menu each command ID went into
static wchar_t got_text[ID_FIRST + MAX_ITEMS][256];
HMENU CreatePopupMenu(void)
{
    int id = ++menus_created;
    m_live[id] = 1;
    menus_live++;
    return (HMENU)(uintptr_t)id;
}
BOOL AppendMenuW(HMENU m, UINT f, UINT_PTR id, LPCWSTR t)
{
    int mi = (int)(uintptr_t)m, len = t ? wlen(t) : -1;
    CHECK(m_live[mi]);
    if (f & MF_POPUP) {
        m_parent[(int)id] = mi;
        sub_len[(int)id] = len;
        n_popups++;
    } else if (f & MF_SEPARATOR) {
        n_separators++;
    } else {
        CHECK(id >= ID_FIRST && id < ID_FIRST + MAX_ITEMS);
        got_flags[id] = f;
        got_len[id] = len;
        got_menu[id] = mi;
        int k = 0;
        for (; t && t[k] && k < 255; k++)
            got_text[id][k] = t[k];
        got_text[id][k] = 0;
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
// By command ID it looks in m and in its submenus, as Windows does.
int GetMenuStringW(HMENU m, UINT id, wchar_t *s, int max, UINT f)
{
    int mi = (int)(uintptr_t)m, n = 0;
    CHECK(f == MF_BYCOMMAND && max > 0 && m_live[mi]);
    int found = id >= ID_FIRST && id < ID_FIRST + MAX_ITEMS && m_live[got_menu[id]] &&
                (got_menu[id] == mi || below(got_menu[id], mi));
    CHECK(found);                           // wintray asks only for items its menu has
    for (; found && got_text[id][n] && n < max - 1; n++)
        s[n] = got_text[id][n];
    s[n] = 0;
    return n;
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
static HWND foreground;
BOOL TrackPopupMenu(HMENU m, UINT f, int x, int y, int r, HWND h, const void *rc)
{
    (void)x; (void)y; (void)r; (void)rc;
    track_calls++;
    CHECK(!on_screen);                      // never two menus at once
    CHECK(h != NULL && m != NULL);
    CHECK(foreground == h);                 // or the menu stays when the user clicks elsewhere
    on_screen = (int)(uintptr_t)m;
    end_menu_called = 0;
    run_hook(&track_hook);                  // things that happen while the menu is open
    on_screen = 0;
    foreground = NULL;                      // so the next menu needs it again
    if (end_menu_called)
        return 0;
    return f & TPM_RETURNCMD ? track_ret : TRUE;   // without it, only success or failure
}
BOOL EndMenu(void) { end_menu_called = 1; return TRUE; }

/* ---------------- icons ---------------- */
#define SHARED ((HICON)(uintptr_t)0xABCDE0)
static int i_live[MAXM], icons_created, icons_live;
static int icon_in_dll, icon_in_exe = 1, from_dll, from_exe;
static int last_res, file_loads, last_path_len;   // the resource ID last loaded, and the files
HANDLE LoadImageW(HINSTANCE h, LPCWSTR name, UINT type, int cx, int cy, UINT fl)
{
    (void)type;
    CHECK(cx == 16 && cy == 16);            // the small icon size
    int ok = 0, res = (int)(uintptr_t)name;
    if (fl & LR_LOADFROMFILE) {
        file_loads++;
        last_path_len = wlen(name);
        ok = name[0] == 'g';                // "good.ico" exists
    } else if (h == DLL) {
        ok = icon_in_dll && res == 1;       // the DLL has icon 1
        from_dll += ok;
    } else if (h == EXE) {
        ok = icon_in_exe && (res == 1 || res == 2);   // the exe has icons 1 and 2
        from_exe += ok;
    }
    if (ok && !(fl & LR_LOADFROMFILE))
        last_res = res;
    if (!ok)
        return NULL;
    int id = ++icons_created;
    i_live[id] = 1;
    icons_live++;
    return (HANDLE)(uintptr_t)id;
}
HICON LoadIconW(HINSTANCE h, LPCWSTR n) { return !h && n == MAKEINTRESOURCEW(32512) ? SHARED : NULL; }
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
// Messages that only the fakes and tests use.
#define WM_CREATE          0x0001
#define WM_CLOSE           0x0010
#define WM_QUERYENDSESSION 0x0011
#define WM_NCCREATE        0x0081
#define WM_SYSKEYDOWN      0x0104
#define WM_SYSCOMMAND      0x0112
#define SC_CLOSE           0xF060
#define VK_F4              0x73
struct cls { int used; LPCWSTR name; HINSTANCE inst; WNDPROC proc; UINT style; int windows; };
static struct cls classes[8];
static struct cls *find_class(LPCWSTR name, HINSTANCE inst)
{
    for (int i = 0; i < (int)ARRAYSIZE(classes); i++)
        if (classes[i].used && classes[i].inst == inst && wsame(classes[i].name, name))
            return &classes[i];
    return NULL;
}
unsigned short RegisterClassW(const WNDCLASSW *wc)
{
    if (find_class(wc->lpszClassName, wc->hInstance))
        return 0;                           // ERROR_CLASS_ALREADY_EXISTS
    for (int i = 0; i < (int)ARRAYSIZE(classes); i++)
        if (!classes[i].used) {
            classes[i] = (struct cls){1, wc->lpszClassName, wc->hInstance, wc->lpfnWndProc, wc->style, 0};
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
struct win { int alive, dying; struct cls *c; };   // alive until DestroyWindow starts
static struct win wins[1000];
static int nwins;
static HWND timer_hwnd;                     // the window timer that is set, if any
static UINT_PTR timer_id;
#define WIN(h) wins[(uintptr_t)(h) / 16]
static int fail_create;                     // CreateWindowW fails, as when out of resources
HWND CreateWindowW(LPCWSTR cls, LPCWSTR title, DWORD st, int x, int y, int w, int h,
                   HWND parent, HMENU menu, HINSTANCE inst, void *param)
{
    (void)title; (void)st; (void)x; (void)y; (void)w; (void)h; (void)parent; (void)menu; (void)param;
    struct cls *c = find_class(cls, inst);
    if (!c || fail_create)
        return NULL;
    HWND hw = (HWND)(uintptr_t)(++nwins * 16);
    WIN(hw) = (struct win){1, 0, c};
    c->windows++;
    if (!c->proc(hw, WM_NCCREATE, 0, 0)) {  // FALSE: no window
        WIN(hw).alive = 0;
        c->windows--;
        return NULL;
    }
    if (c->proc(hw, WM_CREATE, 0, 0) == -1) {
        DestroyWindow(hw);
        return NULL;
    }
    return hw;
}
BOOL DestroyWindow(HWND hw)
{
    CHECK(!WIN(hw).dying);                  // not again while it is being destroyed
    if (!WIN(hw).alive)
        return FALSE;
    WIN(hw).alive = 0;
    WIN(hw).dying = 1;
    WIN(hw).c->proc(hw, WM_DESTROY, 0, 0);
    WIN(hw).c->proc(hw, WM_NCDESTROY, 0, 0);
    WIN(hw).dying = 0;
    WIN(hw).c->windows--;
    KillTimer(hw, timer_id);                // a window's timers go with it
    return TRUE;
}

/* ---------------- Explorer ---------------- */
// Explorer keeps icons by window and ID, and works on a copy of the request
// taken when the call starts, before the messages handled while it waits.
static enum { DOWN, UP, SLOW } explorer = UP;   // SLOW: does the work, but reports a timeout
static struct { HWND hwnd; UINT uid, cbmsg; HICON icon; wchar_t tip[128]; } shown[8];   // the icons in the tray
static int nshown, ghosts, notify_calls;
#define NO_CALL ((DWORD)-1)
static DWORD notify_on = NO_CALL;           // the call during which notify_hook runs
static void (*notify_hook)(void);
static int find_icon(HWND h, UINT uid)
{
    for (int i = 0; i < nshown; i++)
        if (shown[i].hwnd == h && shown[i].uid == uid)
            return i;
    return -1;
}
static void forget_icons(void) { nshown = 0; }   // Explorer restarted
BOOL Shell_NotifyIconW(DWORD m, NOTIFYICONDATAW *d)
{
    NOTIFYICONDATAW r = *d;
    notify_calls++;
    CHECK(r.cbSize == sizeof r);
    if (m != NIM_DELETE && (r.uFlags & NIF_ICON))   // a live icon, not NULL or destroyed
        CHECK(r.hIcon == SHARED || (r.hIcon && i_live[(int)(uintptr_t)r.hIcon]));
    if (m == notify_on)
        run_hook(&notify_hook);             // messages handled while it waits for Explorer
    int i = find_icon(r.hWnd, r.uID);
    if (m == NIM_DELETE) {
        if (i >= 0)
            shown[i] = shown[--nshown];
        return i >= 0;
    }
    if (!WIN(r.hWnd).alive)
        ghosts++;                           // icon added or changed for a closing window
    if (explorer == DOWN)
        return FALSE;
    if (m == NIM_ADD) {
        if (i >= 0)
            return FALSE;                   // already there
        CHECK(nshown < (int)ARRAYSIZE(shown));
        i = nshown++;
        memset(&shown[i], 0, sizeof shown[i]);
        shown[i].hwnd = r.hWnd;
        shown[i].uid = r.uID;
    }
    if (i >= 0) {                           // only the members the flags name
        if (r.uFlags & NIF_MESSAGE)
            shown[i].cbmsg = r.uCallbackMessage;
        if (r.uFlags & NIF_ICON)
            shown[i].icon = r.hIcon;
        if (r.uFlags & NIF_TIP)
            memcpy(shown[i].tip, r.szTip, sizeof r.szTip);
    }
    return i >= 0 && explorer == UP;
}

/* ---------------- the rest ---------------- */
static int thread_timers, quit_posted;
static UINT timer_ms;
UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT ms, void *f)
{
    (void)f;
    if (!h) {
        thread_timers++;
        return 99;
    }
    timer_hwnd = h;
    timer_id = id;
    timer_ms = ms;
    return id;
}
BOOL KillTimer(HWND h, UINT_PTR id)
{
    if (!h || h != timer_hwnd || id != timer_id)
        return FALSE;
    timer_hwnd = NULL;
    return TRUE;
}
BOOL GetCursorPos(POINT *p) { p->x = p->y = 0; return TRUE; }
BOOL SetForegroundWindow(HWND h) { foreground = h; return TRUE; }
static int nulls_posted;
BOOL PostMessageW(HWND h, UINT m, WPARAM w, LPARAM l) { (void)w; (void)l; nulls_posted += h && m == WM_NULL; return TRUE; }
void PostQuitMessage(int c) { (void)c; quit_posted++; }
// What Windows does with the messages wintray leaves to it. Alt+F4 is as in
// Wine, which closes the window unless its class has CS_NOCLOSE; Windows 11
// ignores Alt+F4 for this window either way. Wine posts the SC_CLOSE; here it
// comes at once.
LRESULT DefWindowProcW(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE || m == WM_QUERYENDSESSION)
        return TRUE;                        // go on creating the window; the session may end
    if (m == WM_SYSKEYDOWN && w == VK_F4 && (l & 1 << 29) && !(WIN(h).c->style & CS_NOCLOSE))
        return WIN(h).c->proc(h, WM_SYSCOMMAND, SC_CLOSE, 0);   // Alt+F4
    if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_CLOSE)
        return WIN(h).c->proc(h, WM_CLOSE, 0, 0);
    if (m == WM_CLOSE)
        DestroyWindow(h);
    return 0;
}
#define TASKBAR_CREATED 0xC100
UINT RegisterWindowMessageW(LPCWSTR n) { return wsame(n, L"TaskbarCreated") ? TASKBAR_CREATED : 0xC101; }
static HWND filter_hwnd;                    // the window and message let through UIPI
static UINT filter_msg;
BOOL ChangeWindowMessageFilterEx(HWND h, UINT m, DWORD a, void *p)
{
    (void)p;
    if (a == MSGFLT_ALLOW) {
        filter_hwnd = h;
        filter_msg = m;
    }
    return TRUE;
}
static int get_calls, dispatched;
static MSG pending;                         // a posted message, unless WM_NULL
BOOL PeekMessageW(MSG *m, HWND h, UINT a, UINT b, UINT r)
{
    (void)a; (void)b;
    if (quit_posted) { quit_posted -= (r & PM_REMOVE) != 0; m->message = WM_QUIT; return 1; }
    if (pending.message && (!h || h == pending.hwnd)) {
        *m = pending;
        if (r & PM_REMOVE)
            pending.message = WM_NULL;
        return 1;
    }
    return 0;
}
BOOL GetMessageW(MSG *m, HWND h, UINT a, UINT b)
{
    get_calls++;
    if (PeekMessageW(m, h, a, b, PM_REMOVE))
        return m->message != WM_QUIT;
    m->message = WM_NULL;                   // a message wintray has nothing to do with
    m->hwnd = NULL;
    return 1;
}
BOOL TranslateMessage(const MSG *m) { (void)m; return TRUE; }
LRESULT DispatchMessageW(const MSG *m) { dispatched += m->message != WM_NULL; return 0; }

/* ---------------- to_wide ---------------- */
// Expected result: the longest run of whole characters that fits, split the way
// the decoder above splits the whole text, so no cut changes a character.
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
// rand() gives only 15 bits with MinGW, so two calls, or most code points
// above U+8800 would never come up in a test run on Windows.
static unsigned rand_wide(void) { return (unsigned)rand() << 15 ^ (unsigned)rand(); }
static unsigned rand_cp(void)
{
    unsigned c;
    switch (rand() % 4) {
    case 0: return 1 + rand() % 0x7F;
    case 1: return 0x80 + rand() % (0x800 - 0x80);
    case 2: do c = 0x800 + rand_wide() % (0x10000 - 0x800); while (c >= 0xD800 && c <= 0xDFFF); return c;
    default: return 0x10000 + rand_wide() % (0x110000 - 0x10000);
    }
}

static void test_to_wide(void)
{
    printf("to_wide\n");
    static char s[1400];
    static wchar_t a[300], r[300];
    static const int small[] = {1, 2, 3, 4, 5, 6, 8, 13, 32}, big[] = {128, 256, 260};
    int bad_ref = 0, bad_inv = 0;
    srand(1);
    for (int it = 0; it < 21000; it++) {            // valid UTF-8, all character sizes
        int bigcase = it >= 20000;
        int size = bigcase ? big[rand() % ARRAYSIZE(big)] : small[rand() % ARRAYSIZE(small)];
        int target = rand() % ((bigcase ? 1200 : 120) + 1), len = 0;
        while (len < target)
            len += put_utf8(rand_cp(), s + len);
        s[len] = 0;
        to_wide(s, a, size);
        ref_conv(s, size, r);
        bad_ref += !wsame(a, r);
    }
    // Half the bytes are the ones where UTF-8's ranges start and end.
    static const unsigned char edge[] = {0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0,
                                         0xC1, 0xC2, 0xDF, 0xE0, 0xED, 0xEF, 0xF0, 0xF4, 0xF5, 0xFF};
    for (int it = 0; it < 20000; it++) {            // random bytes, mostly not valid UTF-8
        int size = small[rand() % ARRAYSIZE(small)], len = rand() % 150;
        for (int i = 0; i < len; i++)
            s[i] = (char)(rand() % 2 ? edge[rand() % ARRAYSIZE(edge)] : 1 + rand() % 255);
        s[len] = 0;
        to_wide(s, a, size);
        ref_conv(s, size, r);
        bad_inv += !wsame(a, r);
    }
    CHECK(bad_ref == 0);
    CHECK(bad_inv == 0);

    // Each lead byte with the second bytes at the edges of what it allows,
    // checked directly, so no random sequence has to hit them.
    static const unsigned char lead[] = {0xC1, 0xC2, 0xDF, 0xE0, 0xE1, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF4, 0xF5};
    static const unsigned char second[] = {0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0};
    int bad_edge = 0;
    for (int i = 0; i < (int)ARRAYSIZE(lead); i++)
        for (int j = 0; j < (int)ARRAYSIZE(second); j++) {
            unsigned char e[5] = {lead[i], second[j], 0x80, 0x80, 0};
            unsigned c;
            int k = dec(e, 4, 0, &c), valid = c != 0xFFFD || (k == 3 && e[0] == 0xEF);
            bad_edge += utf8_len((const char *)e) != (valid ? k : 0);
        }
    CHECK(bad_edge == 0);

    memset(s, 'a', 1000);
    s[1000] = 0;
    mb_calls = 0; to_wide(s, a, 256);
    printf("  1000 chars cut to 255: %ld conversion calls\n", mb_calls);
    CHECK(mb_calls <= 2);
    mb_calls = 0; to_wide("Exit", a, 256);
    CHECK(mb_calls == 1);                           // shorter than the buffer: no size query

    // Invalid text, where the stray bytes at the cut each convert to U+FFFD.
    memset(s, 'x', 3);                              // "xxx" and 200 stray continuation bytes
    memset(s + 3, 0x80, 200);
    s[203] = 0;
    to_wide(s, a, 128);
    CHECK(wlen(a) == 127);
    memset(s, 'a', 125);                            // a whole character, then 2 stray bytes at the cut
    strcpy(s + 125, "\xC3\xA9\x80\x80" "bbbb");
    to_wide(s, a, 128);
    CHECK(wlen(a) == 127);
    to_wide("A\x80\x80", a, 2);
    CHECK(wlen(a) == 1);

    memset(s, 'a', 126);                            // valid: an emoji that doesn't fit
    strcpy(s + 126, "\xF0\x9F\x98\x80");
    to_wide(s, a, 128);
    CHECK(wlen(a) == 126);                          // dropped whole, not shown as U+FFFD
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
static struct wintray_menu_item *last_item;    // the item the last item_cb got
static void item_cb(struct wintray_menu_item *m)
{
    last_item = m;
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
    CHECK(hwnd == NULL && menus_live == 0 && icons_live == 0 && nshown == 0 && !update_pending);
    CHECK(!track_hook && !cb_hook && !item_hook && !notify_hook);   // every hook ran
    CHECK(thread_timers == 0);                      // no SetTimer(NULL, ...)
    // A failure here must not fail later tests too.
    track_hook = cb_hook = item_hook = notify_hook = NULL;
    notify_on = NO_CALL;
    track_ret = 0;
    if (hwnd)
        wintray_exit();                             // or every later start() fails
    quit_posted = thread_timers = 0;
    memset(m_live, 0, sizeof m_live);               // a leaked menu or icon counts once
    memset(i_live, 0, sizeof i_live);
    menus_live = icons_live = on_screen = 0;
    forget_icons();
    explorer = UP;
    if (fails > before)
        printf("    called from line %d\n", line);
}
#define closed() closed_at(__LINE__, 0)
#define stop() (wintray_exit(), closed())

// Without an icon window, wintray_update() must do nothing at all: no menu, no
// icon, no Shell_NotifyIconW call, no change to nid, and wintray_get_instance()
// stays as it was.
static void update_without_window_at(int line)
{
    static struct wintray other = {NULL, "other", NULL, menu1, 1};
    struct wintray *instance = wintray_get_instance();
    NOTIFYICONDATAW was;
    memcpy(&was, &nid, sizeof nid);
    int before = fails, menus = menus_created, icons = icons_created, calls = notify_calls;
    wintray_update(&other);
    CHECK(menus_created == menus && icons_created == icons && notify_calls == calls);
    CHECK(wintray_get_instance() == instance && !memcmp(&was, &nid, sizeof nid));
    if (fails > before)
        printf("    called from line %d\n", line);
}
#define update_without_window() update_without_window_at(__LINE__)
// A click, as Explorer sends it: with the callback message the icon was given.
static void ev(UINT e)
{
    int i = find_icon(W, 1);
    CHECK(i >= 0);
    wnd_proc(W, i >= 0 ? shown[i].cbmsg : 0, 1, e);
}
static void left_click(void) { ev(WM_LBUTTONDOWN); ev(WM_LBUTTONUP); }
static void second_click(void) { ev(WM_LBUTTONDBLCLK); ev(WM_LBUTTONUP); }
static void right_click(void) { ev(WM_RBUTTONDOWN); ev(WM_RBUTTONUP); }

static void test_clicks(void)
{
    printf("clicks\n");
    update_without_window();                        // before the first wintray_init()
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
static int menus_at_cb;                         // menus_created when do_update ran
static void do_update(void) { menus_at_cb = menus_created; wintray_update(g_tray); }

static struct wintray_menu_item sub1[] = {{"Inner", 0, 0, item_cb, NULL}, {NULL, 0, 0, NULL, NULL}};
static struct wintray_menu_item flagm[] = {
    {"Off", 1, 0, item_cb, NULL}, {"On", 0, 1, item_cb, NULL}, {"-x", 0, 0, item_cb, NULL},
    {"Info", 0, 0, NULL, NULL}, {"Sub", 0, 0, NULL, sub1}, {NULL, 0, 0, NULL, NULL}};
static struct wintray t_flags = {NULL, "t", NULL, flagm, 1};
static void gray_sub(void) { flagm[4].disabled = 1; wintray_update(&t_flags); }

// Items changed in place while the menu is open, then an update, as a timer may do.
// B keeps its check mark.
static struct wintray_menu_item dyn[] = {
    {"A", 0, 0, item_cb, NULL}, {"B", 0, 1, item_cb, NULL}, {NULL, 0, 0, NULL, NULL}};
static struct wintray t_dyn = {NULL, "t", NULL, dyn, 1};
static int other_calls;
static void other_cb(struct wintray_menu_item *m) { last_item = m; other_calls++; }
static struct wintray_menu_item pick[] = {
    {"First", 0, 0, item_cb, NULL}, {"Second", 0, 0, other_cb, NULL}, {"Third", 0, 0, item_cb, NULL},
    {NULL, 0, 0, NULL, NULL}};
static struct wintray t_pick = {NULL, "t", NULL, pick, 1};
static struct wintray t_alt = {NULL, "alt", NULL, menu1, 1};
static char label[8];                       // a text rewritten in place
static void rename_a_quietly(void) { dyn[0].text = "X"; }   // no update
static void rename_a(void) { rename_a_quietly(); wintray_update(&t_dyn); }
static void rewrite_a(void) { strcpy(label, "X"); wintray_update(&t_dyn); }
static void recall_a(void) { dyn[0].cb = other_cb; wintray_update(&t_dyn); }
static void gray_a(void) { dyn[0].disabled = 1; wintray_update(&t_dyn); }
static void check_a(void) { dyn[0].checked = 1; wintray_update(&t_dyn); }
static char copy_a[] = "A";                     // the same text in another buffer
static void move_a(void) { dyn[0].text = copy_a; wintray_update(&t_dyn); }
static void nest_a(void) { dyn[0].submenu = sub1; wintray_update(&t_dyn); }
static void end_at_a(void) { dyn[0].text = NULL; wintray_update(&t_dyn); }   // a NULL text ends the menu
static void end_at_b(void) { dyn[1].text = NULL; wintray_update(&t_dyn); }
static char *heap_b;                            // B's text, freed while the menu is open
static void cut_and_free(void) { dyn[0].text = NULL; free(heap_b); wintray_update(&t_dyn); }
static struct wintray_menu_item dyn2[] = {      // dyn's items in another array
    {"A", 0, 0, item_cb, NULL}, {"B", 0, 1, item_cb, NULL}, {NULL, 0, 0, NULL, NULL}};
static struct wintray t_dyn2 = {NULL, "t", NULL, dyn2, 1};
static void copy_dyn(void) { wintray_update(&t_dyn2); }
static void switch_to_pick(void) { wintray_update(&t_pick); }
static void update_alt(void) { wintray_update(&t_alt); }
static void reset_dyn(void)
{
    dyn[0] = (struct wintray_menu_item){"A", 0, 0, item_cb, NULL};
    wintray_update(&t_dyn);
}

static void test_menu(void)
{
    printf("menu\n");
    n_separators = 0;
    start(&t_menu);
    CHECK(n_separators == 1);                       // "-" is a separator line
    track_calls = 0; track_hook = right_click; right_click();
    CHECK(track_calls == 1 && menu_open == 0);      // clicked again while the menu is open
    int before = menus_created;
    track_hook = hook_update; right_click();
    CHECK(menus_created > before && update_pending == 0);   // rebuilt once it closed
    before = menus_created;
    track_hook = hook_update; track_ret = ID_FIRST; item_hook = do_update; item_calls = 0;
    right_click();
    CHECK(item_calls == 1 && update_pending == 0);  // item cb updates, plus a pending update
    CHECK(menus_at_cb == before + 1 && menus_created == before + 2);   // the pending one first
    track_ret = 0;
    track_hook = hook_exit; right_click();          // wintray_exit() while the menu is open
    closed();
    CHECK(hmenu == NULL);

    start(&t_menu);                                 // the "Exit" item, with an update waiting
    track_hook = hook_update; track_ret = ID_FIRST + 1; item_hook = wintray_exit; item_calls = 0;
    right_click();
    CHECK(item_calls == 1);
    closed();

    start(&t_pick);                                 // the chosen item, with its own callback
    track_ret = ID_FIRST + 1; item_calls = other_calls = 0; last_item = NULL; nulls_posted = 0;
    right_click();
    CHECK(other_calls == 1 && item_calls == 0 && last_item == &pick[1] && nulls_posted == 1);
    stop();

    start(&t_menu);                                 // an update with another struct while open
    track_hook = update_alt; right_click();
    int i = find_icon(W, 1);
    CHECK(wintray_get_instance() == &t_alt && i >= 0 && wsame(shown[i].tip, L"alt"));
    stop();

    n_separators = 0;
    start(&t_flags);                                // what each item looks like
    CHECK(got_flags[ID_FIRST] == MF_GRAYED && got_flags[ID_FIRST + 1] == MF_CHECKED);
    CHECK(got_flags[ID_FIRST + 2] == MF_STRING && n_separators == 0);   // "-x" is an item
    CHECK(got_len[ID_FIRST] == 3 && got_len[ID_FIRST + 2] == 2 && sub_len[menus_created] == 3);
    track_ret = ID_FIRST + 3; item_calls = 0;
    right_click();                                  // an item without a callback
    CHECK(item_calls == 0);
    track_ret = ID_FIRST + 4; right_click();
    CHECK(item_calls == 1 && last_item == &sub1[0]);   // "Inner", in the submenu
    track_hook = hook_update; right_click();
    CHECK(item_calls == 2 && last_item == &sub1[0]);   // an update that changed nothing
    track_hook = gray_sub; right_click();
    CHECK(item_calls == 2);                         // its submenu was grayed out meanwhile
    flagm[4].disabled = 0;
    stop();

    // An update made while the menu was open runs when it closes, before the
    // click, and the click counts only if the same command still shows the same
    // text, state and callback, since the user meant what the menu showed. A
    // change made without an update counts as it is.
    start(&t_dyn);
    item_calls = other_calls = 0;
    track_hook = rename_a; track_ret = ID_FIRST; right_click();
    CHECK(item_calls == 0);                         // "A" was clicked, but it says "X" now
    reset_dyn();
    track_hook = rename_a; track_ret = ID_FIRST + 1; right_click();
    CHECK(item_calls == 1 && last_item == &dyn[1]); // "B" stayed the same
    reset_dyn();
    track_hook = recall_a; track_ret = ID_FIRST; right_click();
    CHECK(item_calls == 1 && other_calls == 0);     // another callback now
    reset_dyn();
    track_hook = gray_a; track_ret = ID_FIRST; right_click();
    CHECK(item_calls == 1);                         // grayed out meanwhile
    reset_dyn();
    track_hook = check_a; track_ret = ID_FIRST; right_click();
    CHECK(item_calls == 1);                         // checked meanwhile
    strcpy(label, "A"); dyn[0].text = label; wintray_update(&t_dyn);
    track_hook = rewrite_a; track_ret = ID_FIRST; right_click();
    CHECK(item_calls == 1);                         // the same buffer, rewritten
    reset_dyn();
    track_hook = move_a; track_ret = ID_FIRST; right_click();
    CHECK(item_calls == 2 && last_item == &dyn[0]); // the same text from another buffer
    reset_dyn();
    track_hook = nest_a; track_ret = ID_FIRST; right_click();
    CHECK(item_calls == 2);                         // "A" opens a submenu now
    reset_dyn();
    track_hook = end_at_a; track_ret = ID_FIRST + 1; right_click();
    CHECK(item_calls == 2 && item_count == 0);      // the menu ends before "B" now
    reset_dyn();
    track_hook = end_at_b; right_click();
    CHECK(item_calls == 2 && update_pending == 0 && item_count == 1);   // dropped, but updated
    heap_b = malloc(2); strcpy(heap_b, "B"); dyn[1].text = heap_b; wintray_update(&t_dyn);
    track_hook = cut_and_free; right_click();
    CHECK(item_calls == 2);                         // and its freed text isn't read
    dyn[1].text = "B"; reset_dyn();
    track_hook = copy_dyn; right_click();
    CHECK(item_calls == 3 && last_item == &dyn2[1]);   // the same item in another array
    track_hook = switch_to_pick; right_click();
    CHECK(item_calls == 3 && other_calls == 0);     // another menu, with "Second" there now
    reset_dyn();
    track_ret = ID_FIRST; right_click();
    CHECK(item_calls == 4);                         // no update while it was open
    track_hook = rename_a_quietly; right_click();
    CHECK(item_calls == 5 && last_item == &dyn[0]); // changed, but no update came: it counts
    stop();
}

static void on_delete(void)
{
    left_click(); right_click();
    wnd_proc(W, TASKBAR_CREATED, 0, 0);
}
static void exit_again(void) { wintray_exit(); CHECK(wintray_init(&t_menu) == -1); }
static void test_closing(void)
{
    printf("closing\n");
    start(&t_cb);
    cb_calls = track_calls = ghosts = 0;
    notify_on = NIM_DELETE; notify_hook = on_delete;   // clicks while the icon is being removed
    stop();
    CHECK(cb_calls == 0 && track_calls == 0 && ghosts == 0);
    update_without_window();                        // after wintray_exit()

    start(&t_menu);                                 // closed from outside
    wnd_proc(W, WM_CLOSE, 0, 0);                    // as taskkill without /f sends it
    CHECK(window == NULL);                          // forgotten once it is really gone
    closed();
    update_without_window();                        // after the window was closed
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

    start(&t_menu);
    CHECK(wnd_proc(W, WM_QUERYENDSESSION, 0, 0) != 0);   // doesn't hold up a shutdown
    right_click();                                  // Alt+F4 after the menu, which leaves
    wnd_proc(W, WM_SYSKEYDOWN, VK_F4, (LPARAM)1 << 29);   // the keyboard focus here
    CHECK(hwnd == W && nshown == 1 && quit_posted == 0);   // doesn't close it
    stop();

    start(&t_menu);                                 // a WM_DESTROY from another program,
    wnd_proc(W, WM_DESTROY, 0, 0);                  // with no DestroyWindow behind it
    CHECK(WIN(W).alive && hwnd == NULL && nshown == 0);
    wintray_exit();                                 // still destroys the window
    CHECK(!WIN(W).alive && find_class(CLASS_NAME, this_module()) == NULL);
    closed();
    start(&t_menu);
    wnd_proc(W, WM_DESTROY, 0, 0);
    closed();
    start(&t_menu);                                 // or the next init does
    stop();
    start(&t_menu);                                 // a WM_NCDESTROY from another program
    wnd_proc(W, WM_NCDESTROY, 0, 0);
    CHECK(window == W);                             // doesn't make wintray forget the window
    stop();

    start(&t_menu);                                 // exit and init again while the
    notify_on = NIM_DELETE; notify_hook = exit_again;   // window is being destroyed
    stop();

    fail_create = 1;                                // no window: init fails cleanly
    CHECK(wintray_init(&t_menu) == -1 && hwnd == NULL);
    fail_create = 0;
}

static void test_icons(void)
{
    printf("icons\n");
    static struct wintray t = {NULL, "tip", NULL, menu1, 1};
    icon_in_dll = 0; icon_in_exe = 1; from_dll = from_exe = 0;
    start(&t);
    CHECK(own_icon && nid.hIcon == own_icon && from_exe == 1 && from_dll == 0);   // icon in the exe
    if (mode_dll) {
        icon_in_dll = 1;
        wintray_update(&t);
        CHECK(own_icon && from_dll == 1 && from_exe == 1);   // the DLL's own icon comes first
    }
    icon_in_dll = icon_in_exe = 0;
    wintray_update(&t);
    CHECK(nid.hIcon == SHARED && !own_icon);       // no icon anywhere: shared default
    wintray_update(&t);                             // and it's never destroyed
    t.icon_id = 0; t.icon_filepath = "good.ico"; wintray_update(&t);
    CHECK(own_icon);
    t.icon_filepath = "bad.ico"; wintray_update(&t);
    CHECK(nid.hIcon == SHARED);
    static char path[MAX_PATH + 1];                 // a path is used whole or not at all:
    memset(path, 'g', MAX_PATH - 1);                // the longest that fits
    file_loads = 0; t.icon_filepath = path; wintray_update(&t);
    CHECK(file_loads == 1 && last_path_len == MAX_PATH - 1 && own_icon);
    path[MAX_PATH - 1] = 'g';                       // one unit longer: not cut short
    wintray_update(&t);
    CHECK(file_loads == 1 && nid.hIcon == SHARED);
    t.icon_filepath = "g\xC3(.ico";                 // not UTF-8: no bytes replaced either
    wintray_update(&t);
    CHECK(file_loads == 1 && nid.hIcon == SHARED);
    t.icon_id = 2; icon_in_exe = icon_in_dll = 1;   // only the exe has icon 2
    wintray_update(&t);
    CHECK(own_icon && last_res == 2);
    t.icon_id = 65537;                              // over 16 bits: no icon, rather than icon 1
    wintray_update(&t);
    CHECK(nid.hIcon == SHARED && !own_icon);
    t.icon_id = 1; icon_in_dll = 0; t.icon_filepath = "good.ico"; file_loads = 0;
    wintray_update(&t);
    CHECK(own_icon && last_res == 1 && file_loads == 0);   // icon_id, not the file
    for (int i = 0; i < 100; i++)
        wintray_update(&t);
    CHECK(icons_live == 1);                         // no leaks
    stop();
    CHECK(gmh_calls == 1);                          // module looked up once
    CHECK(this_module() == (mode_dll ? DLL : EXE));
}

static void close_window(void) { wnd_proc(W, WM_CLOSE, 0, 0); }
static void test_explorer(void)
{
    printf("explorer\n");
    explorer = DOWN;
    start(&t_menu);
    CHECK(timer_hwnd == W && nshown == 0);          // Explorer not running yet: a retry timer
    CHECK(timer_ms >= 1000);                        // every 2 seconds, not in a busy loop
    explorer = UP;
    wnd_proc(W, WM_TIMER, timer_id, 0);             // the timer the library set
    CHECK(nshown == 1 && timer_hwnd == NULL);       // added on retry, and the timer killed
    int calls = notify_calls;
    wnd_proc(W, WM_TIMER, timer_id + 1, 0);         // a timer that isn't the library's
    CHECK(notify_calls == calls);
    CHECK(shown[0].cbmsg == WM_TRAY && shown[0].icon == nid.hIcon && wsame(shown[0].tip, L"tip"));
    CHECK(filter_hwnd == W && filter_msg == TASKBAR_CREATED);   // also when run elevated
    forget_icons();
    wnd_proc(W, TASKBAR_CREATED, 0, 0);
    CHECK(nshown == 1);                             // re-added after an Explorer restart
    stop();

    // Closed while Shell_NotifyIconW waits for Explorer: no icon may be left for the
    // closed window, no further call may add or change one for it (ghosts), and no
    // SetTimer(NULL, ...). Without an icon, NIM_MODIFY fails
    // and NIM_ADD comes next. With Explorer down, NIM_ADD fails too; with Explorer
    // slow, it reports a timeout but the icon is added anyway.
    static const struct { DWORD call; int state; } cases[] = {
        {NIM_MODIFY, UP}, {NIM_ADD, UP}, {NIM_ADD, DOWN}, {NIM_ADD, SLOW}};
    for (int i = 0; i < (int)ARRAYSIZE(cases); i++) {
        int before = fails;
        start(&t_menu);
        if (cases[i].call == NIM_ADD)
            forget_icons();                         // so NIM_MODIFY fails and NIM_ADD follows
        explorer = cases[i].state;
        ghosts = 0;
        notify_on = cases[i].call; notify_hook = close_window;
        wnd_proc(W, TASKBAR_CREATED, 0, 0);
        explorer = UP;
        closed();
        CHECK(ghosts == 1);                         // only the call that was waiting
        if (fails > before)
            printf("    in case %d\n", i);
    }

    start(&t_menu);                                 // closed during the update the menu waited for
    track_hook = hook_update; track_ret = ID_FIRST; item_calls = 0;
    notify_on = NIM_MODIFY; notify_hook = close_window;
    right_click();
    CHECK(item_calls == 0);                         // so the click doesn't run
    closed();
}

static void test_loop(void)
{
    printf("loop\n");
    start(&t_menu);
    get_calls = 0;
    CHECK(wintray_loop(0) == 0 && get_calls == 0);  // nothing waiting, and it doesn't wait
    CHECK(wintray_loop(1) == 0 && get_calls == 1);  // waits in GetMessageW for a message
    dispatched = 0;                                 // a thread timer gets through, once
    pending = (MSG){NULL, WM_TIMER, 7, 0, 0, {0, 0}};
    CHECK(wintray_loop(0) == 0 && dispatched == 1 && !pending.message);
    pending = (MSG){NULL, WM_TIMER, 7, 0, 0, {0, 0}};
    CHECK(wintray_loop(1) == 0 && dispatched == 2 && !pending.message);
    wintray_exit();
    closed_at(__LINE__, 1);                         // the blocking loop sees WM_QUIT too
}

static struct wintray_menu_item selfm[5], top[71], subs[70][4], many[301];
static struct wintray t_self = {NULL, "t", NULL, selfm, 0};
static struct wintray t_top = {NULL, "t", NULL, top, 0};
static struct wintray t_many = {NULL, "t", NULL, many, 0};
static char longtext[301], longtip[201];
static struct wintray_menu_item longm[] = {{longtext, 0, 0, item_cb, NULL}, {NULL, 0, 0, NULL, NULL}};
static struct wintray t_long = {NULL, longtip, NULL, longm, 1};
static struct wintray t_bare = {NULL, NULL, NULL, NULL, 0};
static void test_limits(void)
{
    printf("limits\n");
    CHECK(MAX_ITEMS == 256 && MAX_MENUS == 256 && MAX_DEPTH == 8);   // as wintray.h and README.md say
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

    memset(longtext, 'a', 300);                     // text longer than the buffers
    memset(longtip, 'b', 200);
    start(&t_long);
    int i = find_icon(W, 1);
    CHECK(got_len[ID_FIRST] == 255 && i >= 0 && wlen(shown[i].tip) == 127);
    stop();

    start(&t_bare);                                 // no tooltip and no menu
    track_calls = 0; right_click();
    i = find_icon(W, 1);
    CHECK(track_calls == 1 && i >= 0 && shown[i].tip[0] == 0);
    wintray_update(NULL);                           // ignored
    CHECK(wintray_get_instance() == &t_bare);
    stop();
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "exe";
    setvbuf(stdout, NULL, _IONBF, 0);               // a run that hangs still shows how far it got
    if (strcmp(mode, "exe") && strcmp(mode, "dll") && strcmp(mode, "gmhfail")) {
        printf("unknown mode %s: use exe, dll or gmhfail\n", mode);
        return 2;
    }
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
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
