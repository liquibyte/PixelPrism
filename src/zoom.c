/* zoom.c - Magnifier Widget Implementation
 *
 * Implements a magnifier widget that captures a region around the cursor,
 * scales it up, and displays it in a separate window. Provides crosshair
 * overlay, color sampling, and zoom level control.
 *
 * Features:
 * - Real-time screen capture and magnification (20x default)
 * - Crosshair overlay for precise targeting
 * - Center square highlighting for selected pixel
 * - Image save/load functionality
 * - Keyboard shortcuts (Enter to pick, Escape to cancel)
 * - Mouse click to pick color
 *
 * Internal design notes:
 * - Grabs the pointer while selecting regions; releases once zoom window shows.
 * - Crosshair/cell colors are pulled from config and cached as Pixels.
 * - Zoom surface is a Pixmap updated via XGetImage for portability.
 */

#include "zoom.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <X11/cursorfont.h>

/* ========== INTERNAL CONSTANTS ========== */

#define ZOOM_SRC 0
#define ZOOM_DST 1
#define DATA uint32_t

struct ZoomContext {
	Display *display;
	Screen *screen;
	Window zoom_window;
	Window line;
	Window square;
	GC zoom_gc;
	XImage *zoom_ximage[2];
	int zoom_mag;
	int zoom_width[2];
	int zoom_height[2];
	int created_images;
	int grab_x, grab_y;
	int is_pressed;
	int is_zoom_active;
	unsigned long last_pixel;
	int is_color_picked;
	int is_cancelled;
	unsigned long crosshair_color; // Crosshair/line color
	unsigned long square_color; // Center square color
	int crosshair_show;           // Show crosshair during picking
	int square_show;              // Show square during picking
	int crosshair_show_after_pick; // Keep crosshair visible after picking
	int square_show_after_pick;    // Keep square visible after picking
	Cursor cursor_cross;
	Cursor cursor_normal;
	ZoomActivationCallback activation_callback;
	void *activation_user_data;
};

/* ========== CURSOR HELPERS ========== */
static void zoom_set_cursor(ZoomContext *ctx, Cursor cursor) {
	if (!ctx || !cursor) {
		return;
	}
	XDefineCursor(ctx->display, ctx->zoom_window, cursor);
	XFlush(ctx->display);
}

static void zoom_set_cursor_cross_internal(ZoomContext *ctx) {
	if (ctx && ctx->cursor_cross) {
		zoom_set_cursor(ctx, ctx->cursor_cross);
	}
}

static void zoom_set_cursor_normal_internal(ZoomContext *ctx) {
	if (ctx && ctx->cursor_normal) {
		zoom_set_cursor(ctx, ctx->cursor_normal);
	}
}

/* ========== IMAGE HANDLING ========== */

static void zoom_allocate_images(ZoomContext *ctx) {
	for (int img = 0; img < 2; ++img) {
		ctx->zoom_ximage[img] = XCreateImage(ctx->display, DefaultVisualOfScreen(ctx->screen), (unsigned int)DefaultDepthOfScreen(ctx->screen), ZPixmap, 0, NULL, (unsigned int)ctx->zoom_width[img], (unsigned int)ctx->zoom_height[img], 32, 0);
		if (!ctx->zoom_ximage[img]) {
			fprintf(stderr, "XCreateImage failed\n");
			exit(1);
		}
		size_t sz = (size_t)ctx->zoom_ximage[img]->bytes_per_line *
		            (size_t)ctx->zoom_ximage[img]->height;
		ctx->zoom_ximage[img]->data = (char *)malloc(sz);
		if (!ctx->zoom_ximage[img]->data) {
			fprintf(stderr, "malloc failed for XImage data (%zu bytes)\n", sz);
			exit(1);
		}
	}
	ctx->created_images = 1;
}

static void zoom_destroy_images(ZoomContext *ctx) {
	if (!ctx->created_images) {
		return;
	}
	for (int img = 0; img < 2; ++img) {
		if (ctx->zoom_ximage[img]) {
			// Free data manually since we allocated it with malloc()
			if (ctx->zoom_ximage[img]->data) {
				free(ctx->zoom_ximage[img]->data);
				ctx->zoom_ximage[img]->data = NULL; // Prevent XDestroyImage from double-freeing
			}
			XDestroyImage(ctx->zoom_ximage[img]);
			ctx->zoom_ximage[img] = NULL;
		}
	}
	ctx->created_images = 0;
}

static int zoom_resize(ZoomContext *ctx, int new_width, int new_height) {
	zoom_destroy_images(ctx);

	// Source sampling size is ceil(dst/zoom_mag)
	ctx->zoom_width[ZOOM_SRC] = (new_width + ctx->zoom_mag - 1) / ctx->zoom_mag;
	ctx->zoom_height[ZOOM_SRC] = (new_height + ctx->zoom_mag - 1) / ctx->zoom_mag;
	if (ctx->zoom_width[ZOOM_SRC] < 1) {
		ctx->zoom_width[ZOOM_SRC] = 1;
	}
	if (ctx->zoom_height[ZOOM_SRC] < 1) {
		ctx->zoom_height[ZOOM_SRC] = 1;
	}
	ctx->zoom_width[ZOOM_DST] = ctx->zoom_mag * ctx->zoom_width[ZOOM_SRC];
	ctx->zoom_height[ZOOM_DST] = ctx->zoom_mag * ctx->zoom_height[ZOOM_SRC];

	zoom_allocate_images(ctx);
	return 0;
}

/* ========== OVERLAYS ========== */
static void overlay_apply_mask_pixmap(ZoomContext *ctx, int width, int height) {
	const int halfw = width / 2;
	const int halfh = height / 2;
	const int sz = ctx->zoom_mag;

	// keep coords signed; precompute unsigned size
	const unsigned int rect_w = (sz > 0) ? (unsigned int)(sz - 1) : 0U;
	const unsigned int rect_h = rect_w; // same for height

	// Create 1-bit mask same size as line window
	Pixmap mask = XCreatePixmap(ctx->display, ctx->line, (unsigned int)width, (unsigned int)height, 1U);
	if (!mask) {
		return;
	}
	GC mgc = XCreateGC(ctx->display, mask, 0, NULL);

	// Clear mask to 0 (invisible everywhere)
	XSetForeground(ctx->display, mgc, 0U);
	XFillRectangle(ctx->display, mask, mgc, (int)(halfw - sz / 2), (int)(halfh - sz / 2), rect_w, rect_h);

	// Draw visible parts with FG=1
	XSetForeground(ctx->display, mgc, 1U);

	// Crosshair segments - just the lines, no center rectangle
	XDrawLine(ctx->display, mask, mgc, 0, halfh, halfw - sz / 2 - 1, halfh);
	XDrawLine(ctx->display, mask, mgc, halfw + sz / 2, halfh, width - 1, halfh);
	XDrawLine(ctx->display, mask, mgc, halfw, 0, halfw, halfh - sz / 2 - 1);
	XDrawLine(ctx->display, mask, mgc, halfw, halfh + sz / 2, halfw, height - 1);

	// Apply shape to the crosshair overlay 'line'
	XShapeCombineMask(ctx->display, ctx->line, ShapeBounding, 0, 0, mask, ShapeSet);

	XFreeGC(ctx->display, mgc);
	XFreePixmap(ctx->display, mask);
}

static void create_overlays(ZoomContext *ctx, int width, int height) {
	XSetWindowAttributes attr;
	attr.override_redirect = True;

	// Crosshair overlay (lines via mask) - independent of square
	attr.background_pixel = ctx->crosshair_color;
	ctx->line = XCreateWindow(ctx->display, ctx->zoom_window, 0, 0, (unsigned int)width, (unsigned int)height, 0, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel, &attr);

	// Center square overlay - hollow outline, independent of crosshair
	attr.background_pixel = ctx->square_color;
	ctx->square = XCreateWindow(ctx->display, ctx->zoom_window, 0, 0, 1, 1, 0, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel, &attr);

	// Initial mask + placement of square, and apply hollow mask
	const int halfw = width / 2, halfh = height / 2, sz = ctx->zoom_mag;
	overlay_apply_mask_pixmap(ctx, width, height);
	XMoveResizeWindow(ctx->display, ctx->square, (int)(halfw - sz / 2), (int)(halfh - sz / 2), (unsigned)sz, (unsigned)sz);
	
	// Make square hollow by masking out the center
	if (sz > 2) {
		Pixmap square_mask = XCreatePixmap(ctx->display, ctx->square, (unsigned)sz, (unsigned)sz, 1U);
		GC square_gc = XCreateGC(ctx->display, square_mask, 0, NULL);
		// Fill with visible
		XSetForeground(ctx->display, square_gc, 1U);
		XFillRectangle(ctx->display, square_mask, square_gc, 0, 0, (unsigned)sz, (unsigned)sz);
		// Clear center (make transparent)
		XSetForeground(ctx->display, square_gc, 0U);
		XFillRectangle(ctx->display, square_mask, square_gc, 1, 1, (unsigned)(sz - 2), (unsigned)(sz - 2));
		// Apply mask
		XShapeCombineMask(ctx->display, ctx->square, ShapeBounding, 0, 0, square_mask, ShapeSet);
		XFreeGC(ctx->display, square_gc);
		XFreePixmap(ctx->display, square_mask);
	}

	// Overlays start hidden - shown only during selection
}

/* Rebuild mask + reposition red square on zoom changes or resize */
static void overlays_rebuild(ZoomContext *ctx, int width, int height) {
	const int halfw = width / 2, halfh = height / 2, sz = ctx->zoom_mag;
	overlay_apply_mask_pixmap(ctx, width, height);
	XMoveResizeWindow(ctx->display, ctx->square, (int)(halfw - sz / 2), (int)(halfh - sz / 2), (unsigned)sz, (unsigned)sz);
	
	// Reapply hollow mask to square
	if (sz > 2) {
		Pixmap square_mask = XCreatePixmap(ctx->display, ctx->square, (unsigned)sz, (unsigned)sz, 1U);
		GC square_gc = XCreateGC(ctx->display, square_mask, 0, NULL);
		// Fill with visible
		XSetForeground(ctx->display, square_gc, 1U);
		XFillRectangle(ctx->display, square_mask, square_gc, 0, 0, (unsigned)sz, (unsigned)sz);
		// Clear center (make transparent)
		XSetForeground(ctx->display, square_gc, 0U);
		XFillRectangle(ctx->display, square_mask, square_gc, 1, 1, (unsigned)(sz - 2), (unsigned)(sz - 2));
		// Apply mask
		XShapeCombineMask(ctx->display, ctx->square, ShapeBounding, 0, 0, square_mask, ShapeSet);
		XFreeGC(ctx->display, square_gc);
		XFreePixmap(ctx->display, square_mask);
	}
}

/* ========== MAGNIFICATION CORE ========== */

static int zoom_magnify(ZoomContext *ctx) {
	// Clamp grab region within root window bounds
	int root_w = WidthOfScreen(ctx->screen);
	int root_h = HeightOfScreen(ctx->screen);
	if (ctx->grab_x < 0) {
		ctx->grab_x = 0;
	}
	if (ctx->grab_y < 0) {
		ctx->grab_y = 0;
	}
	if (ctx->grab_x + ctx->zoom_width[ZOOM_SRC] > root_w) {
		ctx->grab_x = root_w - ctx->zoom_width[ZOOM_SRC];
	}
	if (ctx->grab_y + ctx->zoom_height[ZOOM_SRC] > root_h) {
		ctx->grab_y = root_h - ctx->zoom_height[ZOOM_SRC];
	}
	// Grab source
	XGetSubImage(ctx->display, RootWindowOfScreen(ctx->screen), ctx->grab_x, ctx->grab_y, (unsigned int)ctx->zoom_width[ZOOM_SRC], (unsigned int)ctx->zoom_height[ZOOM_SRC], AllPlanes, ZPixmap, ctx->zoom_ximage[ZOOM_SRC], 0, 0);

	// Stride-aware nearest-neighbor upscale
	const int dst_stride = ctx->zoom_ximage[ZOOM_DST]->bytes_per_line / 4;
	for (int y = 0; y < ctx->zoom_height[ZOOM_SRC]; ++y) {
		const DATA *src_row = (const DATA *)(ctx->zoom_ximage[ZOOM_SRC]->data + y * ctx->zoom_ximage[ZOOM_SRC]->bytes_per_line);
		DATA *dst_row0 = (DATA *)(ctx->zoom_ximage[ZOOM_DST]->data + (y * ctx->zoom_mag) * ctx->zoom_ximage[ZOOM_DST]->bytes_per_line);

		DATA *d = dst_row0;
		for (int x = 0; x < ctx->zoom_width[ZOOM_SRC]; ++x) {
			DATA px = src_row[x];
			for (int z = 0; z < ctx->zoom_mag; ++z) {
				*d++ = px;
			}
		}
		for (int vr = 1; vr < ctx->zoom_mag; ++vr) {
			DATA *dst_rowN = dst_row0 + vr * dst_stride;
			memcpy(dst_rowN, dst_row0, (size_t)ctx->zoom_width[ZOOM_DST] * sizeof(DATA));
		}
	}
	XPutImage(ctx->display, ctx->zoom_window, ctx->zoom_gc, ctx->zoom_ximage[ZOOM_DST], 0, 0, 0, 0, (unsigned int)ctx->zoom_width[ZOOM_DST], (unsigned int)ctx->zoom_height[ZOOM_DST]);

	// Keep overlays visible
	XRaiseWindow(ctx->display, ctx->line);
	XRaiseWindow(ctx->display, ctx->square);
	return 0;
}

/* ========== PUBLIC API ========== */

/**
 * @brief Create screen magnifier widget
 *
 * See zoom.h for full documentation.
 * Initializes zoom window with magnification and crosshair overlay.
 */
ZoomContext *zoom_create(Display *dpy, Window parent, int x, int y, int width, int height) {
	ZoomContext *ctx = (ZoomContext *)calloc(1, sizeof(ZoomContext));
	if (!ctx) {
		return NULL;
	}
	ctx->display = dpy;
	ctx->screen = DefaultScreenOfDisplay(ctx->display);

	XSetWindowAttributes attr;
	attr.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;

	ctx->zoom_window = XCreateWindow(ctx->display, parent, x, y, (unsigned int)width, (unsigned int)height, 0, CopyFromParent, InputOutput, CopyFromParent, CWEventMask, &attr);
	XMapWindow(ctx->display, ctx->zoom_window);

	XGCValues xgcv;
	xgcv.subwindow_mode = ClipByChildren;
	xgcv.function = GXcopy;
	ctx->zoom_gc = XCreateGC(ctx->display, ctx->zoom_window, GCFunction | GCSubwindowMode, &xgcv);

	ctx->zoom_mag = ZOOM_MAG;
	ctx->zoom_width[ZOOM_SRC] = 0;
	ctx->zoom_width[ZOOM_DST] = ZOOM_WIDTH;
	ctx->zoom_height[ZOOM_SRC] = 0;
	ctx->zoom_height[ZOOM_DST] = ZOOM_HEIGHT;
	ctx->created_images = 0;
	ctx->is_pressed = 0;
	ctx->is_zoom_active = 0;
	ctx->last_pixel = 0;
	ctx->is_color_picked = 0;
	ctx->is_cancelled = 0;
	ctx->crosshair_color = 0x00c000; // Default: greenish
	ctx->square_color = 0xc00000; // Default: reddish
	ctx->crosshair_show = 1; // Show crosshair during picking by default
	ctx->square_show = 1;    // Show square during picking by default
	ctx->crosshair_show_after_pick = 0; // Hide crosshair after picking by default
	ctx->square_show_after_pick = 0;    // Hide square after picking by default
	ctx->cursor_cross = XCreateFontCursor(ctx->display, XC_tcross);
	ctx->cursor_normal = XCreateFontCursor(ctx->display, XC_left_ptr);

	zoom_resize(ctx, width, height);
	create_overlays(ctx, ctx->zoom_width[ZOOM_DST], ctx->zoom_height[ZOOM_DST]);
	return ctx;
}

void zoom_begin_selection_ctx(ZoomContext *ctx) {
	if (!ctx) {
		return;
	}
	ctx->is_zoom_active = 1;
	ctx->is_pressed = 1;

	// Show overlays for selection
	zoom_show_overlays_ctx(ctx);
	zoom_set_cursor_cross_internal(ctx);

	// Initialize grab region around current cursor position so the zoom
	// window immediately reflects where the user is pointing when
	// selection starts (before any motion events occur).
	Window root, child;
	int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
	unsigned int mask = 0;
	if (XQueryPointer(ctx->display, RootWindowOfScreen(ctx->screen),
	                 &root, &child, &root_x, &root_y, &win_x, &win_y, &mask)) {
		ctx->grab_x = root_x - ctx->zoom_width[ZOOM_SRC] / 2;
		ctx->grab_y = root_y - ctx->zoom_height[ZOOM_SRC] / 2;
		zoom_magnify(ctx);
	}

	// Grab pointer for selection mode
	XGrabPointer(ctx->display, ctx->zoom_window, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
	
	// Grab keyboard to ensure arrow keys and Enter work during selection
	XGrabKeyboard(ctx->display, ctx->zoom_window, True, GrabModeAsync, GrabModeAsync, CurrentTime);
}

void zoom_cancel_selection_ctx(ZoomContext *ctx) {
	if (!ctx) {
		return;
	}
	ctx->is_zoom_active = 0;
	ctx->is_pressed = 0;

	XUngrabPointer(ctx->display, CurrentTime);
	XUngrabKeyboard(ctx->display, CurrentTime);

	// Hide overlays when selection ends
	zoom_hide_overlays_ctx(ctx);
	zoom_set_cursor_normal_internal(ctx);

	ctx->is_cancelled = 1;
}

void zoom_show_overlays_ctx(ZoomContext *ctx) {
	if (!ctx) {
		return;
	}
	// Show crosshair if enabled during picking
	if (ctx->crosshair_show) {
		XMapWindow(ctx->display, ctx->line);
	} else {
		XUnmapWindow(ctx->display, ctx->line);
	}
	// Show square if enabled during picking (raise it above crosshair)
	if (ctx->square_show) {
		XMapRaised(ctx->display, ctx->square);
	} else {
		XUnmapWindow(ctx->display, ctx->square);
	}
	XFlush(ctx->display);
}

void zoom_hide_overlays_ctx(ZoomContext *ctx) {
	if (!ctx) {
		return;
	}
	// Control overlay visibility after picking based on "show-after-pick" settings
	if (ctx->crosshair_show_after_pick) {
		// Keep crosshair visible
		XMapRaised(ctx->display, ctx->line);
	} else {
		// Hide crosshair
		XUnmapWindow(ctx->display, ctx->line);
	}
	if (ctx->square_show_after_pick) {
		// Keep square visible
		XMapRaised(ctx->display, ctx->square);
	} else {
		// Hide square
		XUnmapWindow(ctx->display, ctx->square);
	}
	XFlush(ctx->display);
}

void zoom_set_cursor_cross(ZoomContext *ctx) {
	if (!ctx) {
		return;
	}
	XDefineCursor(ctx->display, ctx->zoom_window, ctx->cursor_cross);
}

void zoom_set_cursor_normal(ZoomContext *ctx) {
	if (!ctx) {
		return;
	}
	XDefineCursor(ctx->display, ctx->zoom_window, ctx->cursor_normal);
}

unsigned long zoom_get_last_pixel_ctx(ZoomContext *ctx) {
	if (!ctx) {
		return 0;
	}
	ctx->is_color_picked = 0;
	return ctx->last_pixel;
}

int zoom_color_picked_ctx(ZoomContext *ctx) {
	return ctx ? ctx->is_color_picked : 0;
}

int zoom_was_cancelled_ctx(ZoomContext *ctx) {
	if (!ctx) {
		return 0;
	}
	int result = ctx->is_cancelled;
	ctx->is_cancelled = 0;
	return result;
}

/* ========== EVENT LOOP ========== */

/**
 * @brief Process X11 events for zoom widget
 *
 * See zoom.h for full documentation.
 * Handles expose, button press, and keyboard events.
 */
int zoom_handle_event(ZoomContext *ctx, XEvent *ev) {
	if (!ctx) {
		return 0;
	}
	switch (ev->type) {
		case KeyPress: {
			KeySym ks = XkbKeycodeToKeysym(ctx->display, (KeyCode)ev->xkey.keycode, 0, 0);
			unsigned int mods = ev->xkey.state;
			
			// Check for activation shortcut (Ctrl+Alt+Z) - works anytime
			if ((mods & ControlMask) && (mods & Mod1Mask) && (ks == XK_Z || ks == XK_z)) {
				// Activate zoom selection
				zoom_begin_selection_ctx(ctx);
				// Notify application via callback
				if (ctx->activation_callback) {
					ctx->activation_callback(ctx, ctx->activation_user_data);
				}
				return 1;
			}
			
			// Keyboard controls when zoom is active
			if (ctx->is_zoom_active && ctx->is_pressed) {
				Window root, child;
				int root_x, root_y, win_x, win_y;
				unsigned int mask;
				
				// Arrow keys for pixel-precise cursor movement
				if (ks == XK_Left || ks == XK_Right || ks == XK_Up || ks == XK_Down) {
					// Get current cursor position
					XQueryPointer(ctx->display, RootWindowOfScreen(ctx->screen), 
					             &root, &child, &root_x, &root_y, &win_x, &win_y, &mask);
					
					// Calculate new position (move by 1 pixel)
					int new_x = root_x;
					int new_y = root_y;
					
					if (ks == XK_Left) new_x--;
					else if (ks == XK_Right) new_x++;
					else if (ks == XK_Up) new_y--;
					else if (ks == XK_Down) new_y++;
					
					// Clamp to screen bounds
					int screen_width = WidthOfScreen(ctx->screen);
					int screen_height = HeightOfScreen(ctx->screen);
					if (new_x < 0) new_x = 0;
					if (new_y < 0) new_y = 0;
					if (new_x >= screen_width) new_x = screen_width - 1;
					if (new_y >= screen_height) new_y = screen_height - 1;
					
					// Move cursor to new position
					XWarpPointer(ctx->display, None, RootWindowOfScreen(ctx->screen), 
					            0, 0, 0, 0, new_x, new_y);
					
					// Update zoom view to follow cursor
					ctx->grab_x = new_x - ctx->zoom_width[ZOOM_SRC] / 2;
					ctx->grab_y = new_y - ctx->zoom_height[ZOOM_SRC] / 2;
					zoom_magnify(ctx);
					
					return 1;
				}
				// Enter/Return key to pick color at current cursor position
				else if (ks == XK_Return || ks == XK_KP_Enter) {
					// Get current cursor position
					XQueryPointer(ctx->display, RootWindowOfScreen(ctx->screen), 
					             &root, &child, &root_x, &root_y, &win_x, &win_y, &mask);
					
					// Clamp to screen bounds
					int screen_width = WidthOfScreen(ctx->screen);
					int screen_height = HeightOfScreen(ctx->screen);
					if (root_x < 0) root_x = 0;
					if (root_y < 0) root_y = 0;
					if (root_x >= screen_width) root_x = screen_width - 1;
					if (root_y >= screen_height) root_y = screen_height - 1;
					
					// Get pixel color at cursor position
					XImage *img = XGetImage(ctx->display, RootWindowOfScreen(ctx->screen), 
					                       root_x, root_y, 1, 1, AllPlanes, ZPixmap);
					if (img) {
						unsigned long pixel = XGetPixel(img, 0, 0);
						XDestroyImage(img);
						
						ctx->last_pixel = pixel;
						ctx->is_color_picked = 1;
					}
					zoom_cancel_selection_ctx(ctx);
					return 1;
				}
			}
			
			// Only handle zoom-specific +/- shortcuts while zoom selection
			// is active; otherwise, let the rest of the application process
			// key presses normally.
			if (ctx->is_zoom_active) {
				if (ks == '+' || ks == '=') {
					// Preserve center before resize
					int center_x = ctx->grab_x + ctx->zoom_width[ZOOM_SRC] / 2;
					int center_y = ctx->grab_y + ctx->zoom_height[ZOOM_SRC] / 2;
				
					ctx->zoom_mag = (ctx->zoom_mag < 100) ? ctx->zoom_mag + 40 : 100;
					zoom_resize(ctx, ctx->zoom_width[ZOOM_DST], ctx->zoom_height[ZOOM_DST]);
					overlays_rebuild(ctx, ctx->zoom_width[ZOOM_DST], ctx->zoom_height[ZOOM_DST]);
				
					// Recenter on same point
					ctx->grab_x = center_x - ctx->zoom_width[ZOOM_SRC] / 2;
					ctx->grab_y = center_y - ctx->zoom_height[ZOOM_SRC] / 2;
					zoom_magnify(ctx);
					return 1;
				}
				else if (ks == '-') {
					// Preserve center before resize
					int center_x = ctx->grab_x + ctx->zoom_width[ZOOM_SRC] / 2;
					int center_y = ctx->grab_y + ctx->zoom_height[ZOOM_SRC] / 2;
				
					ctx->zoom_mag = (ctx->zoom_mag > 20) ? ctx->zoom_mag - 40 : 20;
					zoom_resize(ctx, ctx->zoom_width[ZOOM_DST], ctx->zoom_height[ZOOM_DST]);
					overlays_rebuild(ctx, ctx->zoom_width[ZOOM_DST], ctx->zoom_height[ZOOM_DST]);
				
					// Recenter on same point
					ctx->grab_x = center_x - ctx->zoom_width[ZOOM_SRC] / 2;
					ctx->grab_y = center_y - ctx->zoom_height[ZOOM_SRC] / 2;
					zoom_magnify(ctx);
					return 1;
				}
			}
			return 0;
		}

		case ButtonPress:
			if (!ctx->is_zoom_active) {
				return 0;
			}
			if (ev->xbutton.button == Button1) {
				if (ctx->is_pressed == 1) {
					// Clamp pick to screen
					int xr = ev->xbutton.x_root;
					int yr = ev->xbutton.y_root;
					int root_w = WidthOfScreen(ctx->screen);
					int root_h = HeightOfScreen(ctx->screen);
					if (xr < 0) {
						xr = 0;
					}
					if (yr < 0) {
						yr = 0;
					}
					if (xr >= root_w) {
						xr = root_w - 1;
					}
					if (yr >= root_h) {
						yr = root_h - 1;
					}
					XImage *img = XGetImage(ctx->display, RootWindowOfScreen(ctx->screen), xr, yr, 1, 1, AllPlanes, ZPixmap);
					if (img) {
						unsigned long pixel = XGetPixel(img, 0, 0);
						XDestroyImage(img);

						ctx->last_pixel = pixel;
						ctx->is_color_picked = 1;
					}
					zoom_cancel_selection_ctx(ctx);
				}
			}
			else if (ev->xbutton.button == Button3) {
				zoom_cancel_selection_ctx(ctx);
			}
			else if (ev->xbutton.button == Button4) {
				// Preserve center before resize
				int center_x = ctx->grab_x + ctx->zoom_width[ZOOM_SRC] / 2;
				int center_y = ctx->grab_y + ctx->zoom_height[ZOOM_SRC] / 2;

				ctx->zoom_mag = (ctx->zoom_mag < 100) ? ctx->zoom_mag + 40 : 100;
				zoom_resize(ctx, ctx->zoom_width[ZOOM_DST], ctx->zoom_height[ZOOM_DST]);
				overlays_rebuild(ctx, ctx->zoom_width[ZOOM_DST], ctx->zoom_height[ZOOM_DST]);

				// Recenter on same point
				ctx->grab_x = center_x - ctx->zoom_width[ZOOM_SRC] / 2;
				ctx->grab_y = center_y - ctx->zoom_height[ZOOM_SRC] / 2;
				zoom_magnify(ctx);
			}
			else if (ev->xbutton.button == Button5) {
				// Preserve center before resize
				int center_x = ctx->grab_x + ctx->zoom_width[ZOOM_SRC] / 2;
				int center_y = ctx->grab_y + ctx->zoom_height[ZOOM_SRC] / 2;

				ctx->zoom_mag = (ctx->zoom_mag > 20) ? ctx->zoom_mag - 40 : 20;
				zoom_resize(ctx, ctx->zoom_width[ZOOM_DST], ctx->zoom_height[ZOOM_DST]);
				overlays_rebuild(ctx, ctx->zoom_width[ZOOM_DST], ctx->zoom_height[ZOOM_DST]);

				// Recenter on same point
				ctx->grab_x = center_x - ctx->zoom_width[ZOOM_SRC] / 2;
				ctx->grab_y = center_y - ctx->zoom_height[ZOOM_SRC] / 2;
				zoom_magnify(ctx);
			}
			return 1;

		case MotionNotify:
			if (ctx->is_zoom_active && ctx->is_pressed) {
				// Keep sample area centered under cursor
				ctx->grab_x = ev->xmotion.x_root - ctx->zoom_width[ZOOM_SRC] / 2;
				ctx->grab_y = ev->xmotion.y_root - ctx->zoom_height[ZOOM_SRC] / 2;
				zoom_magnify(ctx);
			}
			return 1;

		case Expose:
			XPutImage(ctx->display, ctx->zoom_window, ctx->zoom_gc, ctx->zoom_ximage[ZOOM_DST], 0, 0, 0, 0, (unsigned int)ctx->zoom_width[ZOOM_DST], (unsigned int)ctx->zoom_height[ZOOM_DST]);
			return 1;
	}
	return 0;
}

int zoom_get_magnification_ctx(const ZoomContext *ctx) {
	if (!ctx) {
		return ZOOM_MAG;
	}
	return ctx->zoom_mag;
}

void zoom_set_magnification_ctx(ZoomContext *ctx, int zoom_mag) {
	if (!ctx) {
		return;
	}
	if (zoom_mag < 20) {
		zoom_mag = 20;
	}
	if (zoom_mag > 100) {
		zoom_mag = 100;
	}
	ctx->zoom_mag = zoom_mag;
	// Recompute buffers and overlays using existing destination size
	zoom_resize(ctx, ctx->zoom_width[ZOOM_DST], ctx->zoom_height[ZOOM_DST]);
	overlays_rebuild(ctx, ctx->zoom_width[ZOOM_DST], ctx->zoom_height[ZOOM_DST]);
}

Window zoom_get_window(ZoomContext *ctx) {
	return ctx ? ctx->zoom_window : 0;
}

void zoom_set_colors(ZoomContext *ctx, unsigned long crosshair_pixel, unsigned long square_pixel) {
	if (!ctx) {
		return;
	}
	ctx->crosshair_color = crosshair_pixel;
	ctx->square_color = square_pixel;
	// Update existing windows if they exist
	if (ctx->line) {
		XSetWindowBackground(ctx->display, ctx->line, crosshair_pixel);
		XClearWindow(ctx->display, ctx->line);
	}
	if (ctx->square) {
		XSetWindowBackground(ctx->display, ctx->square, square_pixel);
		XClearWindow(ctx->display, ctx->square);
	}
	// Force immediate update
	XFlush(ctx->display);
}

void zoom_set_visibility(ZoomContext *ctx, int crosshair_show, int square_show,
                        int crosshair_show_after_pick, int square_show_after_pick) {
	if (!ctx) {
		return;
	}
	ctx->crosshair_show = crosshair_show;
	ctx->square_show = square_show;
	ctx->crosshair_show_after_pick = crosshair_show_after_pick;
	ctx->square_show_after_pick = square_show_after_pick;
}

int zoom_save_image(ZoomContext *ctx, const char *filepath) {
	if (!ctx || !filepath || !ctx->created_images) {
		return -1;
	}
	FILE *f = fopen(filepath, "wb");
	if (!f) {
		return -1;
	}
	// Write header: width, height, bytes_per_line
	int width = ctx->zoom_width[ZOOM_DST];
	int height = ctx->zoom_height[ZOOM_DST];
	int bytes_per_line = ctx->zoom_ximage[ZOOM_DST]->bytes_per_line;
	if (fwrite(&width, sizeof(int), 1, f) != 1 ||
	    fwrite(&height, sizeof(int), 1, f) != 1 ||
	    fwrite(&bytes_per_line, sizeof(int), 1, f) != 1) {
		fclose(f);
		return -1;
	}
	// Write image data
	size_t data_size = (size_t)bytes_per_line * (size_t)height;
	if (fwrite(ctx->zoom_ximage[ZOOM_DST]->data, 1, data_size, f) != data_size) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

int zoom_load_image(ZoomContext *ctx, const char *filepath) {
	if (!ctx || !filepath) {
		return -1;
	}
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		return -1;
	}
	// Read header
	int saved_width, saved_height, saved_bytes_per_line;
	if (fread(&saved_width, sizeof(int), 1, f) != 1 ||
	    fread(&saved_height, sizeof(int), 1, f) != 1 ||
	    fread(&saved_bytes_per_line, sizeof(int), 1, f) != 1) {
		fclose(f);
		return -1;
	}
	// Verify dimensions match current zoom window
	if (saved_width != ctx->zoom_width[ZOOM_DST] ||
	    saved_height != ctx->zoom_height[ZOOM_DST]) {
		fclose(f);
		return -1;
	}
	// Read image data
	size_t data_size = (size_t)saved_bytes_per_line * (size_t)saved_height;
	if (fread(ctx->zoom_ximage[ZOOM_DST]->data, 1, data_size, f) != data_size) {
		fclose(f);
		return -1;
	}
	fclose(f);

	// Display the loaded image
	XPutImage(ctx->display, ctx->zoom_window, ctx->zoom_gc, ctx->zoom_ximage[ZOOM_DST], 0, 0, 0, 0, (unsigned int)ctx->zoom_width[ZOOM_DST], (unsigned int)ctx->zoom_height[ZOOM_DST]);
	XFlush(ctx->display);

	return 0;
}

void zoom_clear_image(ZoomContext *ctx) {
	if (!ctx || !ctx->created_images) {
		return;
	}
	// Fill the image data with black (0)
	size_t data_size = (size_t)ctx->zoom_ximage[ZOOM_DST]->bytes_per_line *
	                   (size_t)ctx->zoom_height[ZOOM_DST];
	memset(ctx->zoom_ximage[ZOOM_DST]->data, 0, data_size);

	// Display the cleared image
	XPutImage(ctx->display, ctx->zoom_window, ctx->zoom_gc, ctx->zoom_ximage[ZOOM_DST], 0, 0, 0, 0, (unsigned int)ctx->zoom_width[ZOOM_DST], (unsigned int)ctx->zoom_height[ZOOM_DST]);
	XFlush(ctx->display);
}

/**
 * @brief Destroy zoom widget and free resources
 *
 * See zoom.h for full documentation.
 */
void zoom_destroy(ZoomContext *ctx) {
	if (!ctx) {
		return;
	}
	zoom_destroy_images(ctx);
	if (ctx->square) {
		XDestroyWindow(ctx->display, ctx->square);
	}
	if (ctx->line) {
		XDestroyWindow(ctx->display, ctx->line);
	}
	if (ctx->zoom_gc) {
		XFreeGC(ctx->display, ctx->zoom_gc);
	}
	if (ctx->cursor_cross) {
		XFreeCursor(ctx->display, ctx->cursor_cross);
	}
	if (ctx->cursor_normal) {
		XFreeCursor(ctx->display, ctx->cursor_normal);
	}
	if (ctx->zoom_window) {
		XDestroyWindow(ctx->display, ctx->zoom_window);
	}
	free(ctx);
}

void zoom_set_activation_callback(ZoomContext *ctx, ZoomActivationCallback callback, void *user_data) {
	if (!ctx) {
		return;
	}
	ctx->activation_callback = callback;
	ctx->activation_user_data = user_data;
}
