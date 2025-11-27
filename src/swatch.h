#ifndef SWATCH_H_
#define SWATCH_H_

/* ========== SWATCH WIDGET INTERFACE ========== */

/**
 * @file swatch.h
 * @brief Color swatch widget interface for X11
 *
 * Simple color display widget that shows a color sample with automatic
 * contrast-aware border selection. The swatch widget is completely
 * self-contained and can be used independently in other applications.
 *
 * Features:
 * - Color display with RGB values
 * - Automatic contrast-aware border colors
 * - Multiple border modes (complementary, split-complementary, triadic, etc.)
 * - Click event handling
 * - Dynamic color updates without recreation
 * - Configurable dimensions
 * - Fully portable - no application-specific dependencies
 *
 * Dependencies:
 * - X11 (Xlib)
 * - config.h (BorderMode and color utilities)
 * - colormath.h (HSV/luminance calculations)
 *
 * Usage:
 *   1. Create swatch: swatch_create(display, parent_window, width, height)
 *   2. Set color: swatch_set_color(swatch, r, g, b)
 *   3. Handle events: swatch_handle_event(swatch, &event, main_window)
 *      - Returns 1 when clicked
 *   4. Reposition: swatch_set_position(swatch, x, y) if needed
 *   5. Cleanup: swatch_destroy(swatch)
 *
 * Thread safety: Not thread-safe (uses X11 APIs)
 * Memory: Caller must call swatch_destroy() to free resources
 */

#include <X11/Xlib.h>

/* ========== SWATCH DIMENSIONS ========== */

/** Default width of swatch widget in pixels */
#define SWATCH_WIDTH 74

/** Default height of swatch widget in pixels */
#define SWATCH_HEIGHT 74

/* ========== SWATCH CONTEXT TYPE ========== */

/**
 * SwatchContext - Opaque swatch widget context
 *
 * The internal structure is hidden to maintain widget independence.
 * Users interact with the swatch through the provided API functions.
 */
typedef struct SwatchContext SwatchContext;

/* ========== LIFECYCLE MANAGEMENT ========== */

/**
 * @brief Create a new color swatch widget
 * @param dpy X11 display connection
 * @param parent Parent window that will contain the swatch
 * @param width Width of swatch widget in pixels
 * @param height Height of swatch widget in pixels
 *
 * @return Pointer to new swatch context, or NULL on failure
 */
SwatchContext *swatch_create(Display *dpy, Window parent, int width, int height);

/**
 * @brief Destroy a swatch widget and free its resources
 * @param ctx Swatch context to destroy
 */
void swatch_destroy(SwatchContext *ctx);

/* ========== WINDOW MANAGEMENT ========== */

/**
 * @brief Get the X11 window handle for a swatch
 * @param swatch_context Swatch context
 *
 * @return X11 Window ID of the swatch widget
 */
Window swatch_get_window(SwatchContext *ctx);

/* ========== EVENT HANDLING ========== */

/**
 * @brief Process an X11 event for the swatch
 * @param swatch_context Swatch context
 * @param event X11 event to process
 * @param main_window Main application window (for color picking)
 *
 * @return 1 if event was handled, 0 otherwise
 */
int swatch_handle_event(SwatchContext *ctx, const XEvent *ev, Window main_window);

/* ========== COLOR MANAGEMENT ========== */

/**
 * @brief Set the display color of the swatch
 * @param swatch_context Swatch context
 * @param pixel_color X11 pixel color value to display
 */
void swatch_set_color(SwatchContext *ctx, unsigned long pixel);

/**
 * @brief Set background color for contrast detection
 * @param swatch_context Swatch context
 * @param background_pixel Background color for contrast calculations
 *
 * This function helps the swatch determine optimal border colors
 * for visibility against different backgrounds.
 */
void swatch_set_background(SwatchContext *ctx, unsigned long bg_pixel);

/* ========== GEOMETRY MANAGEMENT ========== */

/**
 * @brief Move swatch widget to new position
 * @param swatch_context Swatch context
 * @param x_pos New X coordinate
 * @param y_pos New Y coordinate
 */
void swatch_set_position(SwatchContext *ctx, int x, int y);

/**
 * @brief Set border width and radius for swatch
 * @param swatch_context Swatch context
 * @param border_width Border thickness in pixels
 * @param border_radius Corner radius in pixels
 */
void swatch_set_border(SwatchContext *ctx, int border_width, int border_radius);

/**
 * @brief Resize swatch widget to new dimensions
 * @param swatch_context Swatch context
 * @param width New width in pixels
 * @param height New height in pixels
 */
void swatch_resize(SwatchContext *ctx, int width, int height);

/* ========== CONFIGURATION MANAGEMENT ========== */

#include <stdio.h>
#include "config.h"

/**
 * @brief Initialize swatch configuration with default values
 * @param cfg Configuration structure to initialize
 */
void swatch_config_init_defaults(Config *cfg);

/**
 * @brief Parse a swatch configuration key-value pair
 * @param cfg Configuration structure to update
 * @param key Configuration key
 * @param value Configuration value
 */
void swatch_config_parse(Config *cfg, const char *key, const char *value);

/**
 * @brief Write swatch configuration to file
 * @param f File handle to write to
 * @param cfg Configuration structure to write from
 */
void swatch_config_write(FILE *f, const Config *cfg);

/**
 * @brief Initialize swatch widget geometry defaults
 * @param cfg Pointer to main Config struct
 */
void swatch_widget_config_init_defaults(Config *cfg);

/**
 * @brief Parse swatch widget geometry configuration from key-value pair
 * @param cfg Pointer to main Config struct
 * @param key Configuration key
 * @param value Configuration value as string
 */
void swatch_widget_config_parse(Config *cfg, const char *key, const char *value);

/**
 * @brief Write swatch widget geometry configuration to file
 * @param f File pointer to write to
 * @param cfg Pointer to main Config struct
 */
void swatch_widget_config_write(FILE *f, const Config *cfg);

#endif /* SWATCH_H_ */
