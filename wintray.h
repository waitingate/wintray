// wintray.h - minimal Windows tray icon with a popup menu
//
// Based on the API of tray by Serge Zaitsev (https://github.com/zserge/tray)
// and its fork dmikushin/tray, with the names prefixed wintray_.
//
//   - icon from the exe's resources (icon_id) or from an .ico file
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
// The library keeps pointers to the wintray struct and the menu arrays, so they
// must stay valid while the icon exists (static or global).
//
// Limits: one icon per process, 256 menu items, 8 submenu levels, 255 characters
// per menu item and 127 for the tooltip. Windows 7 or later.

#ifndef WINTRAY_H
#define WINTRAY_H

#ifdef __cplusplus
extern "C" {
#endif

struct wintray_menu_item;

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

struct wintray *wintray_get_instance(void);

int  wintray_init(struct wintray *tray);    // 0 on success, -1 on error or if already running
int  wintray_loop(int blocking);            // returns -1 once the tray has been closed
void wintray_update(struct wintray *tray);  // call after changing the icon, tooltip or menu
void wintray_exit(void);                    // removes the icon and posts WM_QUIT

#ifdef __cplusplus
}
#endif

#endif // WINTRAY_H
