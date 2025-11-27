#ifndef MENU_H_
#define MENU_H_

/* ========== MENU BAR WIDGET INTERFACE ========== */

/**
 * @file menu.h
 * @brief Menu bar widget interface for X11
 *
 * Themeable horizontal menu bar widget with dropdown submenus. The menu widget
 * provides a traditional desktop application menu with configurable items and
 * full theming support. Completely self-contained and can be used independently
 * in other applications.
 *
 * Features:
 * - Horizontal menu bar with mouse hover highlighting
 * - Dropdown submenus with click detection
 * - Configurable menu items (File, Edit, About, or custom)
 * - Full theming support via MiniTheme
 * - Automatic dropdown positioning
 * - Keyboard navigation (arrow keys, Escape)
 * - Optional double-buffer extension (DBE) for smooth rendering
 * - Fully portable - no application-specific dependencies
 *
 * Dependencies:
 * - X11 (Xlib, Xft for text rendering)
 * - config.h (MiniTheme for styling, color/font utilities)
 * - dbe.h (optional double-buffering)
 *
 * Usage:
 *   1. Create menu: menubar_create(display, parent, width, &theme, &config)
 *   2. Handle events: menubar_handle_event(menu, &event) in main loop
 *      - Returns menu item ID when clicked (1-based indexing)
 *   3. Resize: menubar_resize(menu, new_width) if window resizes
 *   4. Update theme: menubar_set_theme(menu, &new_theme)
 *   5. Cleanup: menubar_destroy(menu)
 *
 * Thread safety: Not thread-safe (uses X11 APIs)
 * Memory: Caller must call menubar_destroy() to free resources
 */

#include <X11/Xlib.h>
#include "config.h"

/* ========== TYPE DEFINITIONS ========== */

/* Opaque handle to menu bar instance */
typedef struct MenuBar MenuBar;

/* Menu item configuration structure
 * Defines the labels and counts for each top-level menu's dropdown items.
 */
typedef struct {
	const char *file_items[4]; /* Menu items for File menu */
	const char *edit_items[4]; /* Menu items for Edit menu */
	const char *about_items[4]; /* Menu items for About menu */
	int file_count; /* Number of items in File menu */
	int edit_count; /* Number of items in Edit menu */
	int about_count; /* Number of items in About menu */
} MenuConfig;

/* ========== LIFECYCLE MANAGEMENT ========== */

/**
 * @brief Create a menubar with custom menu configuration (self-contained)
 * @param dpy X11 display
 * @param parent Parent window
 * @param style Menu styling (colors and fonts only)
 * @param x X position
 * @param y Y position
 * @param width Menubar width
 * @param border_width Border width in pixels
 * @param border_radius Border radius in pixels
 * @param padding Internal padding in pixels
 * @param config Menu configuration (items per menu)
 * @return Pointer to MenuBar structure, or NULL on error
 */
MenuBar *menubar_create_with_config(Display *dpy, Window parent, const struct MenuBlock *style, int x, int y, int width, int border_width, int border_radius, int padding, const MenuConfig *config);

/**
 * Create a menubar with default configuration (for backward compatibility)
 * @param dpy X11 display connection
 * @param parent_window Parent window for the menubar
 * @param menubar_theme Theme styling for the menubar
 * @return Pointer to MenuBar instance, or NULL on failure
 */
MenuBar *menubar_create(Display *dpy, Window parent, const MiniTheme *theme);

/**
 * Destroy a menubar and free all associated resources
 * @param menubar Pointer to MenuBar instance (can be NULL)
 */
void menubar_destroy(MenuBar *bar);

/* ========== CONFIGURATION MANAGEMENT ========== */

#include <stdio.h>

void menu_config_init_defaults(struct MenuBlock *block, const char *menu_type);
void menu_config_parse(struct MenuBlock *block, const char *key, const char *value);
void menu_config_write(FILE *f, const struct MenuBlock *block, const char *menu_type);

/**
 * @brief Initialize menubar widget geometry defaults
 * @param cfg Pointer to main Config struct
 */
void menubar_widget_config_init_defaults(Config *cfg);

/**
 * @brief Parse menubar widget geometry configuration from key-value pair
 * @param cfg Pointer to main Config struct
 * @param key Configuration key
 * @param value Configuration value as string
 */
void menubar_widget_config_parse(Config *cfg, const char *key, const char *value);

/**
 * @brief Write menubar widget geometry configuration to file
 * @param f File pointer to write to
 * @param cfg Pointer to main Config struct
 */
void menubar_widget_config_write(FILE *f, const Config *cfg);

/* ========== DRAWING & DISPLAY ========== */

/**
 * Draw/redraw the menubar on screen
 * @param menubar Pointer to MenuBar instance
 */
void menubar_draw(MenuBar *bar);

/**
 * Hide all open submenus (useful when focus moves to other widgets)
 * @param menubar Pointer to MenuBar instance
 */
void menubar_hide_all_submenus(MenuBar *bar);

/* ========== EVENT HANDLING ========== */

/**
 * Handle X11 events for the menubar
 * @param menubar Pointer to MenuBar instance
 * @param event Pointer to XEvent structure
 * @return Action code: 0+ for File items, 100+ for Edit items, 200+ for About items, -1 if no action
 */
int menubar_handle_event(MenuBar *bar, XEvent *ev);

/* ========== WINDOW MANAGEMENT ========== */

/**
 * Get menubar window (for event matching)
 * @param menubar Pointer to MenuBar instance
 * @return Window ID of menubar
 */
Window menubar_get_window(const MenuBar *bar);

/**
 * Check if a window is part of the menubar system (menubar or any submenu)
 * @param menubar Pointer to MenuBar instance
 * @param window Window ID to check
 * @return 1 if window is part of menubar system, 0 otherwise
 */
int menubar_is_menubar_window(const MenuBar *bar, Window win);

/* ========== THEME MANAGEMENT ========== */

/**
 * Apply new theme to menubar and redraw
 * @param menubar Pointer to MenuBar instance
 * @param theme Pointer to new MiniTheme
 */
void menubar_set_theme(MenuBar *bar, const MiniTheme *theme);

/**
 * Move menubar to new position
 * @param menubar Pointer to MenuBar instance
 * @param x New X coordinate
 * @param y New Y coordinate
 */
void menubar_set_position(MenuBar *bar, int x, int y);

#endif /* MENU_H_ */
