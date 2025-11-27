#ifndef TRAY_H_
#define TRAY_H_

#include <X11/Xlib.h>
#include <stdio.h>
#include "config.h"

/**
 * @file tray.h
 * @brief System tray icon widget interface for X11
 *
 * Implements freedesktop.org system tray specification for X11.
 * Provides minimize-to-tray functionality with icon embedding.
 * Completely self-contained and can be used independently in other applications.
 *
 * Features:
 * - System tray icon embedding (freedesktop.org spec)
 * - XEMBED protocol support
 * - Click event handling
 * - Automatic tray detection
 * - Icon from XPM data
 * - Fully portable - no application-specific dependencies
 *
 * Dependencies:
 * - X11 (Xlib)
 * - XPM (icon format)
 *
 * Usage:
 *   1. Check availability: tray_is_available(display, screen)
 *   2. Create tray: tray_create(display, screen, icon_xpm)
 *   3. Handle events: tray_handle_event(tray, &event) in main loop
 *      - Returns 1 when tray icon is clicked
 *   4. Cleanup: tray_destroy(tray)
 *
 * Thread safety: Not thread-safe (uses X11 APIs)
 * Memory: Caller must call tray_destroy() to free resources
 */

#include <X11/Xlib.h>

/* Opaque tray context */
typedef struct TrayContext TrayContext;

/* ========== LIFECYCLE MANAGEMENT ========== */

/**
 * @brief Create system tray icon
 * @param dpy X11 display connection
 * @param screen Screen number
 * @param icon_xpm XPM icon data (e.g., from .xpm file)
 * @param menu_theme Theme configuration for context menu (can be NULL for defaults)
 * @param main_window Main application window (for checking visibility state)
 *
 * Creates and embeds a system tray icon. Automatically locates the system
 * tray and embeds the icon using the XEMBED protocol.
 *
 * @return Tray context or NULL on failure (no tray available)
 */
TrayContext *tray_create(Display *dpy, int screen, const char **icon_xpm, const void *menu_theme, Window main_window);

/**
 * @brief Destroy tray icon and free resources
 * @param ctx Tray context
 */
void tray_destroy(TrayContext *ctx);

/* ========== EVENT HANDLING ========== */

/**
 * @brief Process X11 event for tray icon
 * @param ctx Tray context
 * @param event X11 event to process
 *
 * Handles events for the tray icon window. Call this in your main event loop
 * for any events related to the tray icon window.
 *
 * Return codes:
 *   0 - No action
 *   1 - Left-click on icon (toggle window)
 *   2 - Pick Color menu item
 *   3 - Show/Hide Window menu item
 *   4 - Copy as Hex menu item
 *   5 - Exit menu item
 */
int tray_handle_event(TrayContext *ctx, XEvent *event);

/* ========== VISIBILITY CONTROL ========== */

/**
 * @brief Check if system tray is available
 * @param dpy X11 display connection
 * @param screen Screen number
 *
 * @return 1 if system tray is available, 0 otherwise
 */
int tray_is_available(Display *dpy, int screen);

/**
 * @brief Get tray icon window ID
 * @param ctx Tray context
 *
 * @return Window ID of the tray icon
 */
Window tray_get_window(TrayContext *ctx);

/**
 * @brief Set tray theme
 * @param ctx Tray context
 * @param theme MiniTheme to set
 */
void tray_set_theme(TrayContext *ctx, const MiniTheme *theme);

/* ========== CONFIGURATION MANAGEMENT ========== */

/**
 * @brief Initialize tray configuration defaults
 * @param cfg Configuration to initialize
 */
void tray_config_init_defaults(Config *cfg);

/**
 * @brief Parse tray configuration key-value pair
 * @param cfg Configuration to parse
 * @param key Configuration key
 * @param value Configuration value
 */
void tray_config_parse(Config *cfg, const char *key, const char *value);

/**
 * @brief Write tray configuration to file
 * @param f File to write to
 * @param cfg Configuration to write
 */
void tray_config_write(FILE *f, const Config *cfg);

#endif /* TRAY_H_ */
