/**
 * @file dbe.h
 * @brief X11 Double Buffer Extension helper API
 *
 * Provides helper functions for using the X11 Double Buffer Extension (DBE)
 * to eliminate flicker during window resizing operations.
 *
 * Features:
 * - DBE extension detection and initialization
 * - Back buffer allocation and management
 * - Atomic buffer swapping for flicker-free updates
 * - Graceful fallback when DBE is not available
 */

#ifndef DBE_H_
#define DBE_H_

#include <X11/Xlib.h>
#include <X11/extensions/Xdbe.h>

/* ========== DBE CONTEXT TYPE ========== */

/**
 * DbeContext - Double Buffer Extension context
 *
 * Encapsulates DBE state and functionality for a display connection.
 * All DBE operations should use this context.
 */
typedef struct {
	Display *dpy;
	int screen;
	int dbe_supported; /* 1 if DBE is available, 0 otherwise */
	int major_version;
	int minor_version;
	XdbeScreenVisualInfo *visual_info;
	int num_visuals;
} DbeContext;

/* ========== LIFECYCLE MANAGEMENT ========== */

/**
 * @brief Initialize DBE context for a display
 * @param dpy X11 display connection
 * @param screen Screen number (use DefaultScreen(dpy) for default)
 * @return Pointer to new DBE context, or NULL if initialization fails
 */
DbeContext *dbe_init(Display *dpy, int screen);

/**
 * @brief Destroy DBE context and free resources
 * @param ctx DBE context to destroy
 */
void dbe_destroy(DbeContext *ctx);

/* ========== BUFFER MANAGEMENT ========== */

/**
 * @brief Allocate a back buffer for a window
 * @param ctx DBE context
 * @param window Window to create back buffer for
 * @param swap_action Preferred swap action (usually XdbeUndefined)
 * @return Back buffer ID, or None if allocation fails
 */
XdbeBackBuffer dbe_allocate_back_buffer(DbeContext *ctx, Window window, XdbeSwapAction swap_action);

/**
 * @brief Deallocate a back buffer
 * @param ctx DBE context
 * @param buffer Back buffer to deallocate
 * @return 1 on success, 0 on failure
 */
int dbe_deallocate_back_buffer(DbeContext *ctx, XdbeBackBuffer buffer);

/* ========== SWAPPING OPERATIONS ========== */

/**
 * @brief Swap buffers for a single window
 * @param ctx DBE context
 * @param window Window whose buffers should be swapped
 * @param swap_action How the swap should be performed
 * @return 1 on success, 0 on failure
 */
int dbe_swap_buffers(DbeContext *ctx, Window window, XdbeSwapAction swap_action);

/**
 * @brief Swap buffers for multiple windows atomically
 * @param ctx DBE context
 * @param swap_info Array of swap specifications
 * @param num_windows Number of windows in swap_info array
 * @return 1 on success, 0 on failure
 */
int dbe_swap_buffers_multi(DbeContext *ctx, XdbeSwapInfo *swap_info, int num_windows);

/* ========== UTILITY FUNCTIONS ========== */

/**
 * @brief Check if DBE is supported
 * @param ctx DBE context
 * @return 1 if DBE is supported, 0 otherwise
 */
int dbe_is_supported(DbeContext *ctx);

/**
 * @brief Get visual information for DBE-capable visuals
 * @param ctx DBE context
 * @return Pointer to visual info array, or NULL if none available
 */
XdbeScreenVisualInfo *dbe_get_visual_info(const DbeContext *ctx, int *num_visuals);

#endif /* DBE_H_ */
