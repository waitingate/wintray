// wintray.h - minimal Windows tray icon with a popup menu
//
// Based on the API of tray by Serge Zaitsev (https://github.com/zserge/tray)
// and its fork dmikushin/tray, with the names prefixed wintray_.
//
//   - icon from a resource (icon_id) or from an .ico file
//   - UTF-8 strings
//   - the icon is re-added after an Explorer restart, and retried if adding fails
//   - wintray_update() and wintray_exit() are safe to call while the menu is open
//   - no leftover icon when the window is closed from outside
//   - the left-click callback runs once on a double-click
//   - no C runtime needed
//
// Compile wintray.c with your program and link user32 and shell32.
//
// Call everything from the thread that called wintray_init(). wintray_loop()
// also runs thread timers (SetTimer with a NULL window), so a timer callback is
// a good place for periodic work, including wintray_update().
//
// To show the icon again after wintray_exit(), call wintray_init() once
// wintray_loop() has returned -1, not from inside a callback or timer.
//
// The library keeps pointers to the wintray struct, the menu arrays and their
// strings, so they must stay valid while the icon exists (static or global).
//
// wintray_update() does nothing while there is no icon window: before
// wintray_init(), and after wintray_exit() or the window being closed, until the
// next wintray_init(). So code that fills in the tooltip and menu may run before
// the icon is shown, on the thread that will call wintray_init().
//
// Limits: one icon per process, 256 menu items, 256 submenus, 8 submenu levels,
// 255 UTF-16 units of text per menu item and 127 for the tooltip (most emoji take
// two). Longer text is cut between characters, which can split an emoji made of
// several, such as a flag. An icon file path can have 259 units; a longer one,
// or one that isn't valid UTF-8, loads no icon. Windows 7 or later.

#ifndef WINTRAY_H
#define WINTRAY_H

#include <stddef.h>                     // NULL, which ends the menu arrays

#ifdef __cplusplus
extern "C" {
#endif

struct wintray_menu_item;

struct wintray {
    const char *icon_filepath;          // .ico file (full path), used if icon_id is 0
    const char *tooltip;                // UTF-8
    void (*cb)(struct wintray *);       // left click; if NULL, left click opens the menu
    struct wintray_menu_item *menu;     // ends with an item whose text is NULL
    int icon_id;                        // icon resource ID, or 0
};

struct wintray_menu_item {
    const char *text;                   // UTF-8, "-" for a separator
    int disabled;                       // 1: grayed out
    int checked;                        // 1: check mark
    void (*cb)(struct wintray_menu_item *);    // called when the item is clicked
    struct wintray_menu_item *submenu;  // NULL, or an array like the main menu
};

int  wintray_init(struct wintray *tray);    // 0 on success, -1 on error or if already running
int  wintray_loop(int blocking);            // handles one message (1: waits for one); -1 once closed
void wintray_update(struct wintray *tray);  // call after changing the icon, tooltip or menu
void wintray_exit(void);                    // removes the icon and posts WM_QUIT

// The struct that the last wintray_init() or wintray_update() took (an update
// without an icon window takes nothing); NULL before the first wintray_init().
struct wintray *wintray_get_instance(void);

#ifdef __cplusplus
}
#endif

#endif // WINTRAY_H
