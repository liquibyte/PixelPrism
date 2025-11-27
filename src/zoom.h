/* ========== ZOOM WIDGET INTERFACE ========== */

/**
 * @file zoom.h
 * @brief Screen magnifier and color picker widget interface for X11
 *
 * Screen magnification widget with color picking capabilities. Displays
 * a magnified view of the screen area under the cursor with crosshairs.
 * Completely self-contained and can be used independently in other applications.
 *
 * Features:
 * - Screen magnification with configurable zoom level
 * - Crosshair overlay for precise pixel selection
 * - Color picking from screen
 * - Keyboard activation (Ctrl+Alt+Z) and navigation (arrow keys)
 * - Image load/save support (X11 XImage format)
 * - Selection mode with visual feedback
 * - Shaped window with transparency
 * - Fully portable - no application-specific dependencies
 *
 * Dependencies:
 * - X11 (Xlib, XShape extension)
 *
 * Usage:
 *   1. Create zoom: zoom_create(display, parent, x, y, width, height)
 *   2. Handle events: zoom_handle_event(zoom, &event) in main loop
 *   3. Update magnifier: zoom_magnify(zoom, mouse_x, mouse_y)
 *   4. Color picking: zoom_begin_selection_ctx(zoom)
 *   5. Get color: zoom_get_last_pixel_ctx(zoom, &r, &g, &b)
 *   6. Cleanup: zoom_destroy(zoom)
 *
 * Thread safety: Not thread-safe (uses X11 APIs)
 * Memory: Caller must call zoom_destroy() to free resources
 */

#ifndef ZOOM_H_
#define ZOOM_H_

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/extensions/shape.h>

/* ========== ZOOM WIDGET CONSTANTS ========== */

/** Default width of zoom widget in pixels */
#define ZOOM_WIDTH 300

/** Default height of zoom widget in pixels */
#define ZOOM_HEIGHT 300

/** Default magnification factor for zoom widget */
#define ZOOM_MAG 20

/* ========== ZOOM CONTEXT TYPE ========== */

/**
 * ZoomContext - Opaque zoom widget context
 *
 * The internal structure is hidden to maintain widget independence.
 * Users interact with the zoom widget through the provided API functions.
 */
typedef struct ZoomContext ZoomContext;

/**
 * ZoomActivationCallback - Called when zoom activation shortcut is triggered
 * @param ctx Zoom context
 * @param user_data User-provided data pointer
 */
typedef void (*ZoomActivationCallback)(ZoomContext *ctx, void *user_data);

/* ========== LIFECYCLE MANAGEMENT ========== */

/**
 * @brief Create a new zoom widget
 * @param dpy X11 display connection
 * @param parent_window Parent window that will contain the zoom widget
 * @param x_pos X coordinate for zoom widget placement
 * @param y_pos Y coordinate for zoom widget placement
 * @param width Width of zoom widget in pixels
 * @param height Height of zoom widget in pixels
 *
 * @return Pointer to new zoom context, or NULL on failure
 */
ZoomContext *zoom_create(Display *dpy, Window parent, int x, int y, int width, int height);

/**
 * @brief Destroy a zoom widget and free its resources
 * @param zoom_context Zoom context to destroy
 */
void zoom_destroy(ZoomContext *ctx);

/* ========== WINDOW MANAGEMENT ========== */

/**
 * @brief Get the X11 window handle for a zoom widget
 * @param zoom_context Zoom context
 *
 * @return X11 Window ID of the zoom widget
 */
Window zoom_get_window(ZoomContext *ctx);

/* ========== EVENT HANDLING ========== */

/**
 * @brief Process an X11 event for the zoom widget
 * @param zoom_context Zoom context
 * @param event X11 event to process
 *
 * @return 1 if event was handled, 0 otherwise
 */
int zoom_handle_event(ZoomContext *ctx, XEvent *ev);

/* ========== SELECTION MANAGEMENT ========== */

/**
 * @brief Begin color selection mode
 * @param zoom_context Zoom context
 *
 * Activates the zoom widget for color picking, showing overlays
 * and enabling pixel selection functionality.
 */
void zoom_begin_selection_ctx(ZoomContext *ctx);

/**
 * @brief Cancel color selection mode
 * @param zoom_context Zoom context
 *
 * Deactivates color selection mode and hides overlays.
 */
void zoom_cancel_selection_ctx(ZoomContext *ctx);

/* ========== OVERLAY MANAGEMENT ========== */

/**
 * @brief Show zoom overlay graphics
 * @param zoom_context Zoom context
 *
 * Displays crosshair and selection overlays on the zoom widget.
 */
void zoom_show_overlays_ctx(ZoomContext *ctx);

/**
 * @brief Hide zoom overlay graphics
 * @param zoom_context Zoom context
 *
 * Hides crosshair and selection overlays from the zoom widget.
 */
void zoom_hide_overlays_ctx(ZoomContext *ctx);

/* ========== COLOR PICKING STATE ========== */

/**
 * @brief Get the most recently picked color
 * @param zoom_context Zoom context
 *
 * @return X11 pixel color value of the last picked pixel
 */
unsigned long zoom_get_last_pixel_ctx(ZoomContext *ctx);

/**
 * @brief Check if a color has been picked
 * @param zoom_context Zoom context
 *
 * @return 1 if a color was successfully picked, 0 otherwise
 */
int zoom_color_picked_ctx(ZoomContext *ctx);

/**
 * @brief Check if selection was cancelled
 * @param zoom_context Zoom context
 *
 * @return 1 if the user cancelled color selection, 0 otherwise
 */
int zoom_was_cancelled_ctx(ZoomContext *ctx);

/**
 * @brief Get current zoom magnification factor
 * @param zoom_context Zoom context
 * @return Current magnification factor (e.g. 20, 60, 100)
 */
int zoom_get_magnification_ctx(const ZoomContext *ctx);

/**
 * @brief Set zoom magnification factor and rebuild internal buffers
 * @param zoom_context Zoom context
 * @param zoom_mag Desired magnification factor (will be clamped to valid range)
 */
void zoom_set_magnification_ctx(ZoomContext *ctx, int zoom_mag);

/**
 * @brief Set overlay colors for crosshair and square
 * @param ctx Zoom context
 * @param crosshair_pixel X11 pixel value for crosshair color
 * @param square_pixel X11 pixel value for square border color
 *
 * Configures the colors used for the zoom widget overlays.
 */
void zoom_set_colors(ZoomContext *ctx, unsigned long crosshair_pixel, unsigned long square_pixel);

/**
 * @brief Set overlay visibility preferences
 * @param ctx Zoom context
 * @param crosshair_show Show crosshair during color picking
 * @param square_show Show square during color picking
 * @param crosshair_show_after_pick Keep crosshair visible after picking
 * @param square_show_after_pick Keep square visible after picking
 *
 * Configures which overlays are shown during and after color picking.
 */
void zoom_set_visibility(ZoomContext *ctx, int crosshair_show, int square_show, 
                        int crosshair_show_after_pick, int square_show_after_pick);

/* ========== IMAGE PERSISTENCE ========== */

/**
 * @brief Save the current zoom image to disk
 * @param zoom_context Zoom context
 * @param filepath Path to save the image to
 *
 * @return 0 on success, -1 on failure
 */
int zoom_save_image(ZoomContext *ctx, const char *filepath);

/**
 * @brief Load a saved zoom image from disk
 * @param zoom_context Zoom context
 * @param filepath Path to load the image from
 *
 * @return 0 on success, -1 on failure
 */
int zoom_load_image(ZoomContext *ctx, const char *filepath);

/**
 * @brief Clear the zoom window to black
 * @param zoom_context Zoom context
 */
void zoom_clear_image(ZoomContext *ctx);

/* ========== CALLBACK MANAGEMENT ========== */

/**
 * @brief Set callback for zoom activation shortcut (Ctrl+Alt+Z)
 * @param ctx Zoom context
 * @param callback Function to call when activation shortcut is pressed
 * @param user_data User data to pass to callback
 *
 * The callback allows the application to be notified when the user
 * triggers zoom activation, e.g., to update UI elements like buttons.
 */
void zoom_set_activation_callback(ZoomContext *ctx, ZoomActivationCallback callback, void *user_data);

#endif /* ZOOM_H_ */
