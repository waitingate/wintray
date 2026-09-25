# wintray

A small C library for Windows tray icons: an icon in the notification area with
a tooltip and a popup menu (check marks, disabled items, separators, submenus).

It's one header and one source file. It only uses the Win32 API and doesn't
need the C runtime.

## Example

```cpp
#include "wintray.h"

static void OnExit(wintray_menu_item*) { wintray_exit(); }
static wintray_menu_item menu[] = {{"Exit", 0, 0, OnExit, nullptr}, {nullptr, 0, 0, nullptr, nullptr}};
static wintray t{nullptr, "My tool", nullptr, menu, 1};   // 1 = icon ID in app.rc

int main()
{
    if (wintray_init(&t) < 0) return 1;
    while (wintray_loop(1) == 0) {}
}
```

`app.rc`:

```
1 ICON "icon.ico"
```

Build with MinGW-w64:

```
windres app.rc -O coff -o res.o
gcc -c -Os wintray.c -o wintray.o
g++ -Os -s -mwindows -static -o app.exe app.cpp wintray.o res.o -luser32 -lshell32
```

With other compilers, add `wintray.c` to your project and link `user32` and
`shell32`. I haven't tried MSVC yet. Without the C runtime, compile with `/GS-`:
MSVC's default stack checks call into the runtime.

## API

```c
struct wintray {
    const char *icon_filepath;          // .ico file (absolute path), used if icon_id is 0
    const char *tooltip;                // UTF-8
    void (*cb)(struct wintray *);       // left click; if NULL, left click opens the menu
    struct wintray_menu_item *menu;     // ends with an item whose text is NULL
    int icon_id;                        // icon resource ID, or 0
};

struct wintray_menu_item {
    const char *text;                   // UTF-8, "-" for a separator
    int disabled;
    int checked;
    void (*cb)(struct wintray_menu_item *);
    struct wintray_menu_item *submenu;  // NULL, or an array like the main menu
};

int  wintray_init(struct wintray *tray);    // 0 on success, -1 on error or if already running
int  wintray_loop(int blocking);            // returns -1 once the tray has been closed
void wintray_update(struct wintray *tray);  // call after changing the icon, tooltip or menu
void wintray_exit(void);                    // removes the icon and posts WM_QUIT
struct wintray *wintray_get_instance(void);
```

## Notes

- Strings are UTF-8. Write `&&` for a `&` in menu text (a single `&` marks the
  shortcut key).
- For the icon, use `icon_id` (a resource in your .rc file) or `icon_filepath`.
  Give an absolute path: the working directory isn't always the exe's folder.
- Call all functions from the thread that called `wintray_init()`. For periodic
  work, use a thread timer, `SetTimer(NULL, 0, ms, callback)`. It runs inside
  `wintray_loop()` and can call `wintray_update()` or `wintray_exit()`, even
  while the menu is open.
- The library keeps pointers to your `wintray` struct and menu arrays, so make
  them static or global.
- If Explorer restarts, the icon is added again automatically.
- If `cb` is set, a left click calls it (once per double-click) and a right
  click opens the menu. Otherwise both open the menu.
- `wintray_exit()` posts `WM_QUIT`, which stops every message loop on the
  thread. A `MessageBox` shown after it in the same callback closes right away.
  To show the icon again, call `wintray_init()` once `wintray_loop()` has
  returned -1. `wintray_loop()` also returns -1 on a `WM_QUIT` from your own
  code, but the icon stays until you call `wintray_exit()`.

## Limits

One icon per process, up to 256 menu items and 64 submenus (8 levels deep),
255 characters per menu item and 127 for the tooltip. Windows 7 or later.

## Coming from zserge/tray or dmikushin/tray

Replace `tray_` with `wintray_` and `struct tray` with `struct wintray`.
Strings are UTF-8 instead of the ANSI code page, `icon_filepath` has to be an
.ico file (the originals use `ExtractIconEx`, which also takes an .exe or
.dll), and `struct wintray` has an extra `icon_id` field at the end.

zserge/tray needs a few more changes: `icon` is now `icon_filepath`,
`struct tray_menu` is now `struct wintray_menu_item` (without the `context`
field), and `struct wintray` has `tooltip` and `cb` before `menu`, so check
your initializers.

## Testing

Tested with MinGW-w64 GCC 13.2 (WinLibs), with the test programs running under
Wine.

## License

MIT, see [LICENSE](LICENSE). The API is based on
[tray](https://github.com/zserge/tray) by Serge Zaitsev.
