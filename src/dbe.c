/* dbe.c - X11 Double Buffer Extension Helper Implementation
 *
 * Implements helper functions for using the X11 Double Buffer Extension (DBE)
 * to eliminate flicker during window resizing operations.
 */

#include "dbe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== LIFECYCLE MANAGEMENT ========== */

DbeContext *dbe_init(Display *dpy, int screen) {
	if (!dpy) {
		return NULL;
	}

	DbeContext *ctx = calloc(1, sizeof(DbeContext));
	if (!ctx) {
		return NULL;
	}

	ctx->dpy = dpy;
	ctx->screen = screen;

	/* Query DBE extension */
	if (!XdbeQueryExtension(dpy, &ctx->major_version, &ctx->minor_version)) {
		/* DBE not supported - this is not an error */
		ctx->dbe_supported = 0;
		return ctx;
	}

	ctx->dbe_supported = 1;

	/* Get visual information for DBE-capable visuals */
	Drawable screen_specifier = RootWindow(dpy, screen);
	ctx->visual_info = XdbeGetVisualInfo(dpy, &screen_specifier, &ctx->num_visuals);
	
	if (!ctx->visual_info || ctx->num_visuals == 0) {
		/* No DBE-capable visuals available */
		ctx->dbe_supported = 0;
		if (ctx->visual_info) {
			XdbeFreeVisualInfo(ctx->visual_info);
			ctx->visual_info = NULL;
		}
		return ctx;
	}

	return ctx;
}

void dbe_destroy(DbeContext *ctx) {
	if (!ctx) {
		return;
	}

	if (ctx->visual_info) {
		XdbeFreeVisualInfo(ctx->visual_info);
	}

	free(ctx);
}

/* ========== BUFFER MANAGEMENT ========== */

XdbeBackBuffer dbe_allocate_back_buffer(DbeContext *ctx, Window window, XdbeSwapAction swap_action) {
	if (!ctx || !ctx->dbe_supported || window == None) {
		return None;
	}

	return XdbeAllocateBackBufferName(ctx->dpy, window, swap_action);
}

int dbe_deallocate_back_buffer(DbeContext *ctx, XdbeBackBuffer buffer) {
	if (!ctx || !ctx->dbe_supported || buffer == None) {
		return 0;
	}

	return XdbeDeallocateBackBufferName(ctx->dpy, buffer);
}

/* ========== SWAPPING OPERATIONS ========== */

int dbe_swap_buffers(DbeContext *ctx, Window window, XdbeSwapAction swap_action) {
	if (!ctx || !ctx->dbe_supported || window == None) {
		return 0;
	}

	XdbeSwapInfo swap_info;
	swap_info.swap_window = window;
	swap_info.swap_action = swap_action;
	
	return XdbeSwapBuffers(ctx->dpy, &swap_info, 1);
}

// cppcheck-suppress unusedFunction
int dbe_swap_buffers_multi(DbeContext *ctx, XdbeSwapInfo *swap_info, int num_windows) {
	if (!ctx || !ctx->dbe_supported || !swap_info || num_windows <= 0) {
		return 0;
	}

	return XdbeSwapBuffers(ctx->dpy, swap_info, num_windows);
}

/* ========== UTILITY FUNCTIONS ========== */

int dbe_is_supported(DbeContext *ctx) {
	return ctx ? ctx->dbe_supported : 0;
}

// cppcheck-suppress unusedFunction
XdbeScreenVisualInfo *dbe_get_visual_info(const DbeContext *ctx, int *num_visuals) {
	if (!ctx || !ctx->dbe_supported) {
		if (num_visuals) {
			*num_visuals = 0;
		}
		return NULL;
	}

	if (num_visuals) {
		*num_visuals = ctx->num_visuals;
	}
	return ctx->visual_info;
}
