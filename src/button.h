#ifndef BUTTON_H_
#define BUTTON_H_

/* ========== BUTTON WIDGET INTERFACE ========== */

/**
 * @file button.h
 * @brief Button widget interface for X11
 *
 * Themeable button widget with hover and press states, rounded borders,
 * configurable colors, and Xft font rendering. The button widget is
 * completely self-contained and can be used independently in other applications.
 *
 * Features:
 * - Three visual states: normal, hover, pressed
 * - Rounded rectangle borders with configurable radius
 * - Xft font rendering with antialiasing
 * - Double-buffer extension (DBE) support for smooth rendering
 * - Dynamic theme updates without recreation
 * - Complete event handling
 *
 * Dependencies:
 * - X11 (Xlib, Xft)
 * - config.h (ButtonBlock styling)
 * - dbe.h (optional double-buffering)
 *
 * Usage:
 *   1. Create button: button_create(display, parent_window, &style, ...)
 *   2. Handle events: button_handle_event(button, &event) in main loop
 *      - Returns 2 when clicked (ButtonRelease)
 *      - Returns 1 when other events handled (press, hover, etc.)
 *   3. Update theme: button_set_theme(button, &new_style) as needed
 *   4. Reposition: button_set_position(button, x, y) if needed
 *   5. Cleanup: button_destroy(button)
 *
 * Thread safety: Not thread-safe (uses X11 APIs)
 * Memory: Caller must call button_destroy() to free resources
 */

#include <X11/Xlib.h>
#include "config.h"

/* ========== BUTTON CONTEXT TYPE ========== */

/**
 * ButtonContext - Opaque button widget context
 *
 * The internal structure is hidden to maintain widget independence.
 * Users interact with the button through the provided API functions.
 */
typedef struct ButtonContext ButtonContext;

/* ==========  LIFECYCLE MANAGEMENT ========== */

/**
 * @brief Create a new button widget
 * @param dpy X11 display pointer
 * @param parent_window Parent window for the button
 * @param button_style CSS style block for button appearance (colors and fonts only)
 * @param width Button width in pixels
 * @param height Button height in pixels
 * @param padding Internal padding in pixels
 * @param border_width Border width in pixels
 * @param hover_border_width Hover border width in pixels
 * @param active_border_width Active border width in pixels
 * @param border_radius Border radius in pixels
 * @return Pointer to new button context, or NULL on failure
 */
ButtonContext *button_create(Display *dpy, Window parent_window, const ButtonBlock *button_style, int width, int height, int padding, int border_width, int hover_border_width, int active_border_width, int border_radius);

/**
 * @brief Destroy a button widget and free its resources
 * @param button_context Button context to destroy
 */
void button_destroy(ButtonContext *button_context);

/* ========== WINDOW MANAGEMENT ========== */

/**
 * @brief Get the X11 window handle for a button
 * @param button_context Button context
 * @return X11 Window ID of the button widget
 */
Window button_get_window(ButtonContext *button_context);

/* ==========  EVENT HANDLING  ========== */

/**
 * @brief Process an X11 event for the button
 * @param button_context Button context
 * @param event X11 event to process
 * @return 2 if button clicked (ButtonRelease), 1 if event handled, 0 if event not for button
 *
 * Handles ButtonPress, ButtonRelease, Expose, EnterNotify, LeaveNotify, and MotionNotify events.
 * Returns 2 on ButtonRelease to signal a click completion, allowing applications to distinguish
 * between press (1) and click (2) events.
 */
int button_handle_event(ButtonContext *button_context, const XEvent *event);

/* ========== DRAWING & APPEARANCE ========== */

/**
 * @brief Redraw the button widget
 * @param button_context Button context to draw
 */
void button_draw(ButtonContext *button_context);

/**
 * @brief Reset button to its default (unpressed) state
 * @param button_context Button context to reset
 *
 * Clears the pressed state and redraws the button in its normal or hover state.
 * Typically called after a button action completes.
 */
void button_reset(ButtonContext *button_context);

/**
 * @brief Manually set button pressed state
 * @param button_context Button context
 * @param pressed 1 to set pressed state, 0 to clear it
 *
 * Directly controls the button's pressed state and redraws. Useful for
 * synchronizing button appearance with external state (e.g., keeping button
 * pressed while an action is in progress).
 */
void button_set_pressed(ButtonContext *button_context, int pressed);

/**
 * @brief Set button label text
 * @param button_context Button context
 * @param label New label text (NULL to clear, will be copied)
 *
 * Sets the button's displayed label text. The text is automatically centered
 * both horizontally and vertically within the button. Pass NULL to hide the label.
 * The button takes ownership of the label string and will free it on destroy.
 */
void button_set_label(ButtonContext *button_context, const char *label);

/* ========== GEOMETRY MANAGEMENT ========== */

/**
 * @brief Move button to new position
 * @param button_context Button context
 * @param x_pos New X coordinate
 * @param y_pos New Y coordinate
 */
void button_set_position(ButtonContext *button_context, int x_pos, int y_pos);

/* ========== THEME MANAGEMENT ========== */

/**
 * @brief Apply new theme to button without recreating
 * @param button_context Button context
 * @param button_style New CSS style block for button appearance
 */
void button_set_theme(ButtonContext *button_context, const ButtonBlock *button_style);

/* ========== CONFIGURATION MANAGEMENT ========== */

/**
 * @brief Initialize button configuration block with default values
 * @param button_cfg Pointer to ButtonBlock to initialize
 * @param default_fg Default foreground color (for text)
 * @param default_bg Default background color
 * @param default_border Default border color
 * @param hover_border Hover state border color
 * @param active_border Active/pressed state border color
 */
void button_config_init_defaults(ButtonBlock *button_cfg);

/**
 * @brief Parse button configuration from key-value pair
 * @param button_cfg Pointer to ButtonBlock to update
 * @param key Configuration key name
 * @param value Configuration value as string
 * @return 1 if key was recognized and parsed, 0 otherwise
 */
int button_config_parse(ButtonBlock *button_cfg, const char *key, const char *value);

/**
 * @brief Write button configuration to file
 * @param f File pointer to write to
 * @param button_cfg Pointer to ButtonBlock to write
 * @return 0 on success, non-zero on error
 */
int button_config_write(FILE *f, const ButtonBlock *button_cfg);

/**
 * @brief Initialize button widget geometry defaults
 * @param cfg Pointer to main Config struct
 */
void button_widget_config_init_defaults(Config *cfg);

/**
 * @brief Parse button widget geometry configuration from key-value pair
 * @param cfg Pointer to main Config struct
 * @param key Configuration key
 * @param value Configuration value as string
 * @return 1 if key was recognized and parsed, 0 otherwise
 */
int button_widget_config_parse(Config *cfg, const char *key, const char *value);

/**
 * @brief Write button widget geometry configuration to file
 * @param f File pointer to write to
 * @param cfg Pointer to main Config struct
 * @return 0 on success, non-zero on error
 */
int button_widget_config_write(FILE *f, const Config *cfg);

#endif /* BUTTON_H_ */
