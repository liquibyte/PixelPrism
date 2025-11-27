#ifndef ENTRY_H_
#define ENTRY_H_

/* ========== ENTRY WIDGET INTERFACE ========== */

/**
 * @file entry.h
 * @brief Text entry widget interface for X11
 *
 * Comprehensive text input widget with validation, undo/redo, selection,
 * clipboard integration, and context menu support. The entry widget is
 * completely self-contained and can be used independently in other applications.
 *
 * Features:
 * - Multiple input types: text, integer, float, hexadecimal
 * - Full keyboard input with text selection
 * - Clipboard integration (Ctrl+C/V/X)
 * - Undo/redo functionality
 * - Right-click context menu (cut/copy/paste/select all/clear/undo/redo)
 * - Cursor positioning and blinking
 * - Validation state tracking (for application use)
 * - Change callbacks for application notification
 * - Batch update variants (_noflush, _no_draw) for performance
 *
 * Dependencies:
 * - X11 (Xlib, Xft)
 * - clipboard.h (copy/paste operations)
 * - context.h (right-click menu)
 * - config.h (EntryBlock styling)
 *
 * Usage:
 *   1. Create entry: entry_create(display, screen, parent, theme, config, clipboard)
 *   2. Handle events: entry_handle_event(entry, &event) in main loop
 *   3. Get/set text: entry_get_text(entry) / entry_set_text(entry, "text")
 *   4. Validation: entry_set_validation_state(entry, state) to track state
 *   5. Timers: entry_update_blink(entry) periodically for cursor blinking
 *   6. Cleanup: entry_destroy(entry)
 *
 * Thread safety: Not thread-safe (uses X11 APIs)
 * Memory: Caller must call entry_destroy() to free resources
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include "config.h"
#include "clipboard.h"

/* ========== ENTRY TYPE DEFINITIONS ========== */

/**
 * enum EntryKind - Types of input validation for entry widgets
 */
typedef enum EntryKind {
	ENTRY_TEXT = 0, /**< Free-form text input */
	ENTRY_INT, /**< Integer numbers only */
	ENTRY_FLOAT, /**< Floating-point numbers */
	ENTRY_HEX /**< Hexadecimal color values */
} EntryKind;

/**
 * MiniEntry - Opaque entry widget context
 *
 * The internal structure is hidden to maintain widget independence.
 * Users interact with the entry through the provided API functions.
 */
typedef struct MiniEntry MiniEntry;

/**
 * MiniEntryCallback - Callback function type for entry change events
 * @param entry Entry widget that triggered the callback
 * @param user_data User-provided data pointer
 */
typedef void (*MiniEntryCallback)(MiniEntry *entry, void *user_data);

/**
 * struct MiniEntryConfig - Configuration for entry widget creation
 * @param kind Type of input validation to apply
 * @param x_pos X coordinate of entry widget
 * @param y_pos Y coordinate of entry widget
 * @param width Width of entry widget
 * @param height Height of entry widget
 * @param max_length Maximum number of characters allowed
 * @param on_change Callback function called when text changes
 * @param user_data User data passed to callback function
 */
typedef struct MiniEntryConfig {
	EntryKind kind;
	int x_pos, y_pos, width;
	int padding;
	int border_width;
	int border_radius;
	int max_length;
	MiniEntryCallback on_change;
	void *user_data;
} MiniEntryConfig;

/* ========== LIFECYCLE MANAGEMENT ========== */

/**
 * @brief Create and initialize a new entry widget
 * @param dpy X11 display connection
 * @param screen Screen number for the display
 * @param parent_window Parent window that will contain the entry
 * @param entry_theme Theme configuration for appearance
 * @param entry_config Configuration structure for entry behavior
 * @param clipboard_ctx Clipboard context for copy/paste operations
 *
 * @return Pointer to new entry context, or NULL on failure
 */
MiniEntry *entry_create(Display *dpy, int screen, Window parent, const MiniTheme *theme, const MiniEntryConfig *cfg, ClipboardContext *clipboard_ctx);

/**
 * @brief Destroy an entry widget and free its resources
 * @param entry Entry context to destroy
 */
void entry_destroy(MiniEntry *e);

/* ========== FOCUS MANAGEMENT ========== */

/**
 * @brief Set focus state of entry widget
 * @param entry Entry context
 * @param is_focused 1 to focus, 0 to unfocus
 */
void entry_focus(MiniEntry *e, int f);

/**
 * @brief Check if an entry is currently focused
 * @param entry Entry context
 * @return 1 if entry is focused, 0 otherwise
 */
int entry_is_focused_check(const MiniEntry *e);

/**
 * @brief Handle window focus changes
 * @param entry Entry context
 * @param has_focus 1 if window gained focus, 0 if lost
 */
void entry_handle_window_focus(MiniEntry *e, int has_focus);

/* ========== DRAWING & APPEARANCE ========== */

/**
 * @brief Redraw the entry widget
 * @param entry Entry context to draw
 */
void entry_draw(MiniEntry *e);

/**
 * @brief Redraw the entry widget without flushing (for batch updates)
 * @param entry Entry context to draw
 */
void entry_draw_noflush(MiniEntry *e);

/**
 * @brief Apply new theme to entry widget
 * @param entry Entry context
 * @param entry_theme New theme configuration
 */
void entry_set_theme(MiniEntry *e, const MiniTheme *theme);

/**
 * @brief Apply new theme to entry widget without flushing (for batch updates)
 * @param entry Entry context
 * @param entry_theme New theme configuration
 */
void entry_set_theme_noflush(MiniEntry *e, const MiniTheme *theme);

/* ========== GEOMETRY MANAGEMENT ========== */

/**
 * @brief Move entry widget to new position
 * @param entry Entry context
 * @param x_pos New X coordinate
 * @param y_pos New Y coordinate
 */
void entry_move(MiniEntry *e, int x, int y);

/**
 * @brief Move entry widget to new position without flushing (for batch updates)
 * @param entry Entry context
 * @param x_pos New X coordinate
 * @param y_pos New Y coordinate
 */
void entry_move_noflush(MiniEntry *e, int x, int y);

/**
 * @brief Change entry widget dimensions
 * @param entry Entry context
 * @param width New width
 * @param height New height
 */
void entry_resize(MiniEntry *e, int w, int h);

/**
 * @brief Change entry widget dimensions without flushing (for batch updates)
 * @param entry Entry context
 * @param width New width
 * @param height New height
 */
void entry_resize_noflush(MiniEntry *e, int w, int h);

/* ========== TEXT MANAGEMENT ========== */

/**
 * @brief Set the text content of entry widget
 * @param entry Entry context
 * @param text New text content (null-terminated string)
 */
void entry_set_text(MiniEntry *e, const char *t);

/**
 * @brief Set text content without redrawing (for batch updates)
 * @param entry Entry context
 * @param text New text content (null-terminated string)
 */
void entry_set_text_no_draw(MiniEntry *e, const char *t);

/**
 * @brief Get current text content from entry widget
 * @param entry Entry context
 *
 * @return Pointer to current text (do not free, valid until entry is modified)
 */
const char *entry_get_text(MiniEntry *e);

/* ========== EVENT HANDLING ========== */

/**
 * @brief Process an X11 event for the entry
 * @param entry Entry context
 * @param event X11 event to process
 *
 * @return 1 if event was handled, 0 otherwise
 *
 * Handles KeyPress, ButtonPress, ButtonRelease, MotionNotify, FocusIn, FocusOut,
 * and Expose events. Supports text input, cursor positioning, text selection,
 * clipboard operations (Ctrl+C/V/X), and context menu (right-click).
 */
int entry_handle_event(MiniEntry *e, XEvent *ev);

/**
 * @brief Handle timed events (cursor blink, etc.)
 * @param entry Entry context
 */
void entry_update_timers(MiniEntry *e);

/**
 * @brief Update cursor blink state
 * @param entry Entry context
 */
void entry_update_blink(MiniEntry *e);

/* ========== CALLBACK MANAGEMENT ========== */

/**
 * @brief Set or change the entry change callback
 * @param entry Entry context
 * @param callback New callback function
 * @param user_data User data to pass to callback
 */
void entry_set_callback(MiniEntry *e, MiniEntryCallback cb, void *user_data);

/* ========== VALIDATION STATE (for application use) ========== */

/**
 * @brief Set validation state (application-defined)
 * @param e Entry context
 * @param state Application-defined state value
 *
 * Stores a validation state value for application use. The widget does not
 * interpret this value - applications can use it to track validation status
 * and apply custom visual feedback if desired.
 */
void entry_set_validation_state(MiniEntry *e, int state);

/**
 * @brief Get current validation state
 * @param e Entry context
 * @return Current validation state value
 */
int entry_get_validation_state(const MiniEntry *e);

/**
 * @brief Get validation flash start time
 * @param e Entry context
 * @return Timestamp in milliseconds
 */
long long entry_get_validation_flash_start(const MiniEntry *e);

/**
 * @brief Set validation flash start time
 * @param e Entry context
 * @param timestamp Timestamp in milliseconds
 */
void entry_set_validation_flash_start(MiniEntry *e, long long timestamp);

/* ========== CONFIGURATION MANAGEMENT ========== */

#include <stdio.h>
#include "config.h"

/**
 * @brief Initialize entry configuration with default values
 * @param block Entry config block to initialize
 * @param entry_type Entry type name for identification
 */
void entry_config_init_defaults(struct EntryBlock *block, const char *entry_type);

/**
 * @brief Parse an entry configuration key-value pair
 * @param block Entry config block to update
 * @param key Configuration key
 * @param value Configuration value
 */
void entry_config_parse(struct EntryBlock *block, const char *key, const char *value);

/**
 * @brief Write entry configuration to file
 * @param f File handle to write to
 * @param block Entry config block to write from
 * @param entry_type Entry type name for section header
 */
void entry_config_write(FILE *f, const struct EntryBlock *block, const char *entry_type);

#endif /* ENTRY_H_ */
