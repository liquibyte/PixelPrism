#ifndef PIXELPRISM_H_
#define PIXELPRISM_H_

/* ========== MAIN APPLICATION INTERFACE ========== */

/**
 * @file pixelprism.h
 * @brief Main application header for PixelPrism
 *
 * PixelPrism: A lightweight X11 color picker and palette tool
 *
 * Main application coordination and high-level API for the color picker
 * application. Manages the main window, event loop, and widget coordination.
 *
 * Usage:
 *   Run: ./pixelprism [config_file]
 *   
 *   Default config: ~/.config/pixelprism/pixelprism.conf
 *   
 *   Main functions:
 *   - refresh_entries(): Update all entry widgets with current color values
 *   - create_about_window(): Display application about dialog
 *   - destroy_about_window(): Close about dialog
 *
 * The application provides a magnifier view, RGB/HSV/HSL entry fields,
 * color swatch, and system tray integration. All widgets are coordinated
 * through the main event loop.
 *
 * Dependencies:
 * - colormath (color space conversions)
 * - entry (text entry widgets)
 * - config (theming and configuration)
 * - zoom (magnifier widget)
 * - X11, Xft (windowing and rendering)
 *
 * This header defines the main application entry point and supporting
 * structures for the about window and entry refreshing.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/Xft/Xft.h>

#include "colormath.h"
#include "entry.h"
#include "config.h"
#include "zoom.h"

/* ========== APPLICATION ENTRY ========== */

/* Main application entry point
 *
 * Initializes X11 connection, creates all widgets, and runs the main
 * event loop. Does not return until application exits.
 */
void pixelprism(void);

/* ========== WINDOW DIMENSIONS ========== */

/* Main window dimensions (pixels) */
#define MAIN_WIDTH 588
#define MAIN_HEIGHT 300

/* ========== ABOUT WINDOW API ========== */

/* Opaque handle to about/help window */
typedef struct AboutWindow AboutWindow;

/* Create the about window
 *
 * @param dpy    X11 Display connection
 * @param parent Parent window
 * @param theme  Theme configuration
 * @return About window instance
 */
AboutWindow *about_create(Display *dpy, Window parent, const MiniTheme *theme);

/**
 * Destroy about window and free resources
 *
 * @param win About window to destroy
 */
void about_destroy(AboutWindow *win);

/**
 * Update about window theme
 *
 * @param win   About window instance
 * @param theme New theme to apply
 */
void about_set_theme(AboutWindow *win, const MiniTheme *theme);

/**
 * Show the about window
 *
 * @param win About window to show
 */
void about_show(AboutWindow *win);

/**
 * Hide the about window
 *
 * @param win About window to hide
 */
void about_hide(AboutWindow *win);

/**
 * Check if about window is visible
 *
 * @param win About window to query
 * @return 1 if visible, 0 if hidden
 */
int about_is_visible(AboutWindow *win);

/**
 * Handle X11 events for about window
 *
 * @param win About window instance
 * @param ev  Event to process
 * @return 1 if event was handled, 0 otherwise
 */
int about_handle_event(AboutWindow *win, XEvent *ev);

/* ========== STATE MANAGEMENT API ========== */

int state_load_window_position(int *x, int *y);
int state_save_window_position(int x, int y);
int state_load_zoom_mag(int *zoom_mag_out);
int state_save_zoom_mag(int zoom_mag);
int state_load_last_color(char hex_out[8]);
int state_save_last_color(const char *hex);

#endif /* PIXELPRISM_H_ */
