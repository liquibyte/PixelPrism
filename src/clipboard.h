#ifndef CLIPBOARD_H_
#define CLIPBOARD_H_

#include <X11/Xlib.h>

/* ========== CLIPBOARD SYSTEM INTERFACE ========== */

/**
 * @file clipboard.h
 *
 *
 * Provides GTK-like API for handling X11 CLIPBOARD and PRIMARY selections.
 * Supports both copy (becoming selection owner) and paste (requesting data).
 *
 * Usage:
 *   1. Create context: clipboard_create(display)
 *   2. Set text: clipboard_set_text(ctx, window, text, type)
 *   3. Request text: clipboard_request_text(ctx, window, callback, data, type)
 *   4. Handle events: clipboard_handle_event(ctx, &event) in main loop
 *   5. Cleanup: clipboard_destroy(ctx)
 */

/* ========== TYPE DEFINITIONS ========== */

typedef struct ClipboardContext ClipboardContext;

/**
 * Callback invoked when clipboard_request_text() completes
 * @param text - The retrieved text (UTF-8), or NULL if request failed
 * @param user_data - User data passed to clipboard_request_text()
 */
typedef void (*ClipboardCallback)(const char *text, void *user_data);

/**
 * Selection type for clipboard operations
 */
typedef enum {
	SELECTION_CLIPBOARD, /* Standard Ctrl+C/V clipboard */
	SELECTION_PRIMARY /* X11 select-to-copy, middle-click-to-paste */
} SelectionType;

/* ========== LIFECYCLE MANAGEMENT ========== */

/**
 * @brief Initialize clipboard system for a Display
 * @param dpy X11 Display connection
 *
 * @return Clipboard context, or NULL on failure
 *
 * Creates a clipboard context that manages selection ownership and requests.
 * One context per Display is typical.
 */
ClipboardContext *clipboard_create(Display *dpy);

/**
 * @brief Clean up clipboard system
 * @param ctx Clipboard context to destroy
 *
 * Frees all resources, including owned clipboard data.
 * Any pending paste requests will be cancelled.
 */
void clipboard_destroy(ClipboardContext *ctx);

/* ========== SELECTION MANAGEMENT ========== */

/**
 * @brief Copy text to clipboard/selection
 * @param ctx Clipboard context
 * @param win Window that will own the selection
 * @param text UTF-8 text to copy (will be duplicated internally)
 * @param type SELECTION_CLIPBOARD or SELECTION_PRIMARY
 *
 * Makes the specified window the owner of the selection.
 * The text will be provided to other applications that request it.
 * Pass NULL for text to clear ownership.
 */
void clipboard_set_text(ClipboardContext *ctx, Window win, const char *text, SelectionType type);

/**
 * @brief Request text from clipboard/selection
 * @param ctx Clipboard context
 * @param win Window requesting the paste
 * @param callback Function to call when data arrives (or NULL on failure)
 * @param user_data Arbitrary data passed to callback
 * @param type SELECTION_CLIPBOARD or SELECTION_PRIMARY
 *
 * Asynchronously requests text from the selection owner.
 * The callback will be invoked when:
 *   - Data arrives successfully (text != NULL)
 *   - Request fails or selection has no owner (text == NULL)
 *
 * You must continue processing events with clipboard_handle_event()
 * for the callback to be invoked.
 */
void clipboard_request_text(ClipboardContext *ctx, Window win, ClipboardCallback callback, void *user_data, SelectionType type);

/* ========== EVENT HANDLING ========== */

/**
 * @brief Process clipboard-related X11 events
 * @param ctx Clipboard context
 * @param ev X11 event to process
 *
 * @return 1 if event was handled, 0 if not clipboard-related
 *
 * Call this from your main event loop to handle:
 *   - SelectionRequest: Someone wants our clipboard data
 *   - SelectionNotify: Response to our paste request
 *   - SelectionClear: We lost clipboard ownership
 *
 * Example:
 *   while (1) {
 *       XNextEvent(display, &event);
 *       if (clipboard_handle_event(clipboard_ctx, &event))
 *           continue;  // Event handled by clipboard
 *       // ... handle other events
 *   }
 */
int clipboard_handle_event(ClipboardContext *ctx, XEvent *ev);

#endif /* CLIPBOARD_H_ */
