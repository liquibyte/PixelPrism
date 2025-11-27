#ifndef LABEL_H_
#define LABEL_H_

/* ========== LABEL WIDGET INTERFACE ========== */

/**
 * @file label.h
 * @brief Label widget interface for X11
 *
 * Simple, themeable text label widget for displaying static or dynamic text
 * with Xft rendering. The label widget is completely self-contained and can
 * be used independently in other applications.
 *
 * Features:
 * - Auto-sizing based on text content and font metrics
 * - Manual sizing override support
 * - Full Xft font rendering with antialiasing
 * - Themeable colors and rounded borders
 * - Dynamic text updates without recreation
 * - Optional double-buffer extension (DBE) for smooth rendering
 * - Show/hide functionality
 * - Fully portable - no application-specific dependencies
 *
 * Dependencies:
 * - X11 (Xlib)
 * - Xft (font rendering with antialiasing)
 * - dbe.h (optional double-buffering)
 *
 * Usage:
 *   1. Create label: label_create(display, screen, parent, x, y, width, ...)
 *   2. Set text: label_set_text(label, "Text") - auto-resizes to fit
 *   3. Update position: label_move(label, x, y)
 *   4. Update theme: label_set_theme(label, &theme)
 *   5. Handle expose: label_handle_expose(label, &expose_event)
 *   6. Cleanup: label_destroy(label)
 *
 * Thread safety: Not thread-safe (uses X11 APIs)
 * Memory: Caller must call label_destroy() to free resources
 */

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

/* ========== TYPE DEFINITIONS ========== */

/* Base theme structure - common theming elements for label widgets
 * This is a minimal theme that doesn't depend on the full MiniTheme.
 * Contains only colors and fonts - geometry is passed at creation.
 */
typedef struct {
	char font_family[128];
	int font_size;
	double fg_r, fg_g, fg_b, fg_a;
	double bg_r, bg_g, bg_b, bg_a;
	double border_r, border_g, border_b, border_a;
} BaseTheme;

/* Label widget context */
typedef struct LabelContext LabelContext;

/* ========== LIFECYCLE MANAGEMENT ========== */

/**
 * @brief Create a new label widget
 * @param dpy X11 display connection
 * @param screen X11 screen number
 * @param parent Parent window
 * @param x X position
 * @param y Y position
 * @param width Label width
 * @param padding Internal padding
 * @param border_width Border width
 * @param border_radius Border radius
 * @param border_enabled Whether border is enabled
 * @param text Text to display
 * @param theme Theme configuration (colors and fonts only)
 *
 * @return Label context pointer, or NULL on failure
 */
LabelContext *label_create(Display *dpy, int screen, Window parent, int x, int y, int width, int padding, int border_width, int border_radius, int border_enabled, const char *text, const BaseTheme *theme);

/**
 * @brief Destroy a label widget
 * @param label Label context to destroy
 */
void label_destroy(LabelContext *label);

/* ========== TEXT & THEME MANAGEMENT ========== */

/**
 * @brief Update label text
 * @param label Label context
 * @param text New text string
 */
void label_set_text(LabelContext *label, const char *text);

/**
 * @brief Update label theme
 * @param label Label context
 * @param theme New theme configuration
 */
void label_set_theme(LabelContext *label, const BaseTheme *theme);

/* ========== GEOMETRY MANAGEMENT ========== */

/**
 * @brief Move label to new position
 * @param label Label context
 * @param x New X position
 * @param y New Y position
 */
void label_move(LabelContext *label, int x, int y);

/**
 * @brief Resize label widget
 * @param label Label context
 * @param width New width (0 for auto-size)
 * @param height New height (0 for auto-size)
 */
void label_resize(LabelContext *label, int width, int height);

/**
 * @brief Update label geometry (borders and padding)
 * @param label Label context
 * @param padding Internal padding
 * @param border_width Border width
 * @param border_radius Border corner radius
 * @param border_enabled Whether border is enabled
 */
void label_set_geometry(LabelContext *label, int padding, int border_width, int border_radius, int border_enabled);

/* ========== WINDOW MANAGEMENT ========== */

/**
 * @brief Get label's X11 window
 * @param label Label context
 *
 * @return X11 window ID
 */
Window label_get_window(LabelContext *label);

/**
 * @brief Show label widget
 * @param label Label context
 */
void label_show(LabelContext *label);

/**
 * @brief Hide label widget
 * @param label Label context
 */
void label_hide(LabelContext *label);

/* ========== EVENT HANDLING ========== */

/**
 * @brief Handle expose event for label
 * @param label Label context
 * @param ev Expose event
 *
 * @return 1 if event was handled, 0 otherwise
 */
int label_handle_expose(LabelContext *label, const XExposeEvent *ev);

/* ========== CONFIGURATION MANAGEMENT ========== */

#include <stdio.h>
#include "config.h"

/* Label configuration structure (forward declaration from config.h) */
typedef struct {
	char font_family[128];
	int font_size;
	ConfigColor fg;
	ConfigColor bg;
	ConfigColor border;
	int padding;
	int border_radius;
	int border_width;
	int border_enabled;
	int width;
	char default_font_family[64];
	int default_font_size;
} LabelConfig;

/**
 * @brief Initialize label configuration block with default values
 * @param label_cfg Pointer to label config to initialize
 * @param default_fg Default foreground color
 * @param default_bg Default background color
 * @param default_border Default border color
 */
void label_config_init_defaults(LabelConfig *label_cfg, ConfigColor default_fg,
                                 ConfigColor default_bg, ConfigColor default_border);

/**
 * @brief Parse label configuration from key-value pair
 * @param label_cfg Pointer to label config to update
 * @param key Configuration key name
 * @param value Configuration value as string
 * @return 1 if key was recognized and parsed, 0 otherwise
 */
int label_config_parse(LabelConfig *label_cfg, const char *key, const char *value);

/**
 * @brief Write label configuration to file
 * @param f File pointer to write to
 * @param label_cfg Pointer to label config to write
 * @return 0 on success, non-zero on error
 */
int label_config_write(FILE *f, const LabelConfig *label_cfg);

/**
 * @brief Get DBE usage status for debugging
 * @param label Pointer to label context
 * @return 1 if DBE is being used, 0 otherwise
 */
int label_is_using_dbe(const LabelContext *label);

#endif /* LABEL_H_ */
