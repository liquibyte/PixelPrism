#ifndef CONTEXT_H_
#define CONTEXT_H_

/* ========== CONTEXT MENU INTERFACE ========== */

/**
 * @file context.h
 * @brief Context menu (popup menu) widget interface for X11
 *
 * Lightweight, themeable popup context menu widget for X11 applications.
 * Displays a vertical menu with configurable items and visual feedback.
 * Completely self-contained and can be used independently in other applications.
 *
 * Features:
 * - Popup menu at any screen position
 * - Themeable via MiniTheme configuration
 * - Hover highlighting and click detection
 * - Automatic positioning and sizing
 * - Configurable menu items with enable/disable states
 * - Auto-hide when clicking outside or selecting item
 * - Optional double-buffer extension (DBE) for smooth rendering
 * - Fully portable - no application-specific dependencies
 *
 * Dependencies:
 * - X11 (Xlib, Xft for text rendering)
 * - config.h (MiniTheme for theming, color/font utilities)
 * - dbe.h (optional double-buffering)
 *
 * Usage:
 *   1. Create menu: menu_create(display, screen, &theme)
 *   2. Show menu: menu_show(menu, x, y, items, count) - displays at position
 *   3. Handle events: menu_handle_event(menu, &event) in main loop
 *      - Returns item index when clicked (0-based), -1 if cancelled
 *   4. Hide menu: menu_hide(menu)
 *   5. Cleanup: menu_destroy(menu)
 *
 * Thread safety: Not thread-safe (uses X11 APIs)
 * Memory: Caller must call menu_destroy() to free resources
 */

#include <X11/Xlib.h>
#include "config.h"

/* ========== TYPE DEFINITIONS ========== */

/* Opaque handle to context menu instance */
typedef struct ContextMenu ContextMenu;

/* ========== LIFECYCLE MANAGEMENT ========== */

/* Create a new context menu instance
 *
 * @param dpy        X11 Display connection
 * @param screen     X11 Screen number
 * @param menu_theme Theme configuration for menu appearance
 * @return Pointer to new ContextMenu, or NULL on allocation failure
 */
ContextMenu *menu_create(Display *dpy, int screen, const MiniTheme *theme);

/* Show the context menu at specified screen coordinates
 *
 * @param context_menu Menu instance to show
 * @param x_pos        X coordinate (screen space)
 * @param y_pos        Y coordinate (screen space)
 */
void menu_show(ContextMenu *m, int x, int y);

/* Hide the context menu (unmap window)
 *
 * @param context_menu Menu instance to hide
 */
void menu_hide(ContextMenu *m);

/* Destroy context menu and free all resources
 *
 * @param context_menu Menu instance to destroy
 */
void menu_destroy(ContextMenu *m);

/* ========== MENU OPERATIONS ========== */

/* Handle X11 events for the context menu
 *
 * Process mouse motion, clicks, and focus events. Returns the index of
 * the selected menu item, or -1 if no item was selected.
 *
 * @param context_menu Menu instance
 * @param event        X11 event to process
 * @param can_cut      Enable/disable Cut item (0=disabled, 1=enabled)
 * @param can_copy     Enable/disable Copy item
 * @param can_paste    Enable/disable Paste item
 * @param can_select_all Enable/disable Select All item
 * @param can_clear    Enable/disable Clear item
 * @param can_undo     Enable/disable Undo item
 * @param can_redo     Enable/disable Redo item
 * @return Menu item index (0-6) or -1 if no selection made
 */
int menu_handle_event(ContextMenu *m, XEvent *ev, int can_cut, int can_copy, int can_paste, int can_select_all, int can_clear, int can_undo, int can_redo);

/* Force immediate redraw of menu with current item states
 *
 * @param context_menu Menu instance to redraw
 * @param can_cut      Current Cut item state
 * @param can_copy     Current Copy item state
 * @param can_paste    Current Paste item state
 * @param can_select_all Current Select All item state
 * @param can_clear    Current Clear item state
 * @param can_undo     Current Undo item state
 * @param can_redo     Current Redo item state
 */
void menu_draw(ContextMenu *m, int can_cut, int can_copy, int can_paste, int can_select_all, int can_clear, int can_undo, int can_redo);

/* ========== WINDOW MANAGEMENT ========== */

/* Check if menu is currently visible
 *
 * @param context_menu Menu instance to query
 * @return 1 if visible, 0 if hidden
 */
int menu_is_visible(ContextMenu *m);

/* Get the X11 window handle for this menu
 *
 * @param context_menu Menu instance
 * @return X11 Window ID
 */
Window menu_get_window(ContextMenu *m);

#endif /* CONTEXT_H_ */
