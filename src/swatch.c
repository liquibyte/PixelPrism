/* swatch.c - Color Swatch Widget Implementation
 *
 * Implements the main color swatch display that shows the currently selected
 * color. Supports configurable border styles, rounded corners, and optional
 * checkerboard background for transparency visualization.
 *
 * Internal design notes:
 * - Swatch is a child window with its own GC to avoid flickering.
 * - Checkerboard background is precomputed into a Pixmap for reuse.
 * - Border mode changes trigger a redraw but no geometry changes.
 *
 * Features:
 * - Automatic contrast-based border selection
 * - Rounded rectangle with configurable radius
 * - Click-to-pick color functionality
 * - Dynamic color updates
 * - Background-aware border rendering
 * - Shaped window with rounded corners
 */

#include "swatch.h"
#include "config.h"
#include "dbe.h"
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/shape.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Opaque type definition */
struct SwatchContext {
	Display *display;
	int screen;
	Window parent;
	Window swatch_window;

	int swatch_width;
	int swatch_height;

	int moving;
	int root_x, root_y;
	int swatch_x, swatch_y;

	int initial_x, initial_y;
	int attached;
	unsigned long last_pixel;
	unsigned long main_bg_pixel;

	int border_width;
	int border_radius;
	
	// DBE support
	DbeContext *dbe_ctx;
	XdbeBackBuffer dbe_back_buffer;
	int use_dbe; // 1 if DBE is available and initialized
};

/* ========== CURSOR HELPERS ========== */

static void cursor_invisible(SwatchContext *ctx) {
	Pixmap blank;
	Colormap pointer_map;
	Cursor no_pointer;
	XColor black, dummy;
	static char blank_data[8] = {
		0
	};

	pointer_map = DefaultColormap(ctx->display, ctx->screen);
	XAllocNamedColor(ctx->display, pointer_map, "black", &black, &dummy);
	blank = XCreateBitmapFromData(ctx->display, ctx->swatch_window, blank_data, 8, 8);
	no_pointer = XCreatePixmapCursor(ctx->display, blank, blank, &black, &black, 0, 0);

	XDefineCursor(ctx->display, ctx->swatch_window, no_pointer);
	XFreeCursor(ctx->display, no_pointer);
	if (blank != None) {
		XFreePixmap(ctx->display, blank);
	}
	XFreeColors(ctx->display, pointer_map, &black.pixel, 1, 0);
}

static void cursor_normal(SwatchContext *ctx) {
	Cursor c = XCreateFontCursor(ctx->display, XC_left_ptr);
	XDefineCursor(ctx->display, ctx->swatch_window, c);
	XFreeCursor(ctx->display, c);
}

/* ========== COLOR HELPERS ========== */

static unsigned long color_complementary(Display *dpy, int screen, unsigned long pixel) {
	XColor c = {0};
	c.pixel = pixel;
	XQueryColor(dpy, DefaultColormap(dpy, screen), &c);
	c.red = (unsigned short)(65535 - c.red);
	c.green = (unsigned short)(65535 - c.green);
	c.blue = (unsigned short)(65535 - c.blue);
	c.flags = DoRed | DoGreen | DoBlue;
	XAllocColor(dpy, DefaultColormap(dpy, screen), &c);
	return c.pixel;
}

static unsigned long color_inverse(Display *dpy, int screen, unsigned long pixel) {
	XColor c = {0};
	c.pixel = pixel;
	XQueryColor(dpy, DefaultColormap(dpy, screen), &c);
	// Find max and min channels
	unsigned short max_val = c.red;
	unsigned short min_val = max_val;
	if (c.green > max_val) {
		max_val = c.green;
	}
	if (c.blue > max_val) {
		max_val = c.blue;
	}
	if (c.green < min_val) {
		min_val = c.green;
	}
	if (c.blue < min_val) {
		min_val = c.blue;
	}
	// Invert by swapping with opposite end of range
	c.red = (unsigned short)(max_val + min_val - c.red);
	c.green = (unsigned short)(max_val + min_val - c.green);
	c.blue = (unsigned short)(max_val + min_val - c.blue);
	c.flags = DoRed | DoGreen | DoBlue;
	XAllocColor(dpy, DefaultColormap(dpy, screen), &c);
	return c.pixel;
}

static unsigned long color_contrast(Display *dpy, int screen, unsigned long pixel) {
	XColor c = {0};
	c.pixel = pixel;
	XQueryColor(dpy, DefaultColormap(dpy, screen), &c);
	double lum = (0.2126 * (double)c.red + 0.7152 * (double)c.green + 0.0722 * (double)c.blue) / 65535.0;
	return (lum > 0.5) ? BlackPixel(dpy, screen) : WhitePixel(dpy, screen);
}

static unsigned long color_triadic(Display *dpy, int screen, unsigned long pixel) {
	XColor c = {0};
	c.pixel = pixel;
	XQueryColor(dpy, DefaultColormap(dpy, screen), &c);
	unsigned short r = c.red, g = c.green, b = c.blue;
	c.red = g;
	c.green = b;
	c.blue = r;
	c.flags = DoRed | DoGreen | DoBlue;
	XAllocColor(dpy, DefaultColormap(dpy, screen), &c);
	return c.pixel;
}

static double color_distance(Display *dpy, int screen, unsigned long p1, unsigned long p2) {
	XColor c1 = {0}, c2 = {0};
	c1.pixel = p1;
	c2.pixel = p2;
	XQueryColor(dpy, DefaultColormap(dpy, screen), &c1);
	XQueryColor(dpy, DefaultColormap(dpy, screen), &c2);
	double dr = (c1.red - c2.red) / 65535.0;
	double dg = (c1.green - c2.green) / 65535.0;
	double db = (c1.blue - c2.blue) / 65535.0;
	return sqrt(dr * dr + dg * dg + db * db);
}

static unsigned long enhanced_border_color(Display *dpy, int screen, unsigned long swatch_px, unsigned long main_bg_px) {
	double dist_to_bg = color_distance(dpy, screen, swatch_px, main_bg_px);
	double dist_threshold = 0.25;
	if (dist_to_bg < dist_threshold) {
		return color_contrast(dpy, screen, main_bg_px);
	}
	BorderMode mode = config_get_border_mode();
	switch (mode) {
		case BORDER_MODE_COMPLEMENTARY:
			return color_complementary(dpy, screen, swatch_px);
		case BORDER_MODE_INVERSE:
			return color_inverse(dpy, screen, swatch_px);
		case BORDER_MODE_CONTRAST:
			return color_contrast(dpy, screen, swatch_px);
		case BORDER_MODE_TRIADIC:
		default:
			return color_triadic(dpy, screen, swatch_px);
	}
}

/* ========== ROUNDED RECTANGLE DRAWING ========== */

static void draw_rounded_rect(Display *dpy, Drawable d, GC gc, int x, int y, int w, int h, int radius) {
	if (radius <= 0 || radius * 2 > w || radius * 2 > h) {
		XDrawRectangle(dpy, d, gc, x, y, (unsigned int)(w - 1), (unsigned int)(h - 1));
		return;
	}
	int diameter = radius * 2;

	// Draw four arcs for corners
	XDrawArc(dpy, d, gc, x, y, (unsigned int)diameter, (unsigned int)diameter, 90 * 64, 90 * 64);
	XDrawArc(dpy, d, gc, x + w - diameter - 1, y, (unsigned int)diameter, (unsigned int)diameter, 0, 90 * 64);
	XDrawArc(dpy, d, gc, x, y + h - diameter - 1, (unsigned int)diameter, (unsigned int)diameter, 180 * 64, 90 * 64);
	XDrawArc(dpy, d, gc, x + w - diameter - 1, y + h - diameter - 1, (unsigned int)diameter, (unsigned int)diameter, 270 * 64, 90 * 64);

	// Draw four lines connecting the arcs
	XDrawLine(dpy, d, gc, x + radius, y, x + w - radius - 1, y);
	XDrawLine(dpy, d, gc, x + w - 1, y + radius, x + w - 1, y + h - radius - 1);
	XDrawLine(dpy, d, gc, x + w - radius - 1, y + h - 1, x + radius, y + h - 1);
	XDrawLine(dpy, d, gc, x, y + h - radius - 1, x, y + radius);
}

/* ========== FILLED ROUNDED RECTANGLE FOR SHAPE MASK ========== */

static void fill_rounded_rect(Display *dpy, Drawable d, GC gc, int x, int y, int w, int h, int radius) {
	if (radius <= 0 || radius * 2 > w || radius * 2 > h) {
		XFillRectangle(dpy, d, gc, x, y, (unsigned int)w, (unsigned int)h);
		return;
	}
	int diameter = radius * 2;
	// Fill interior - slightly smaller to ensure outline pixels are on top
	if (w > 2 && h > 2) {
		XFillRectangle(dpy, d, gc, x + 1, y + 1, (unsigned int)(w - 2), (unsigned int)(h - 2));
	}
	// Draw the EXACT same outline as draw_rounded_rect using XDrawArc and XDrawLine
	// This ensures the shape mask includes precisely the pixels the border touches
	XSetLineAttributes(dpy, gc, 1, LineSolid, CapButt, JoinMiter);

	// Draw four arcs for corners - IDENTICAL to draw_rounded_rect
	XDrawArc(dpy, d, gc, x, y, (unsigned int)diameter, (unsigned int)diameter, 90 * 64, 90 * 64);
	XDrawArc(dpy, d, gc, x + w - diameter - 1, y, (unsigned int)diameter, (unsigned int)diameter, 0, 90 * 64);
	XDrawArc(dpy, d, gc, x, y + h - diameter - 1, (unsigned int)diameter, (unsigned int)diameter, 180 * 64, 90 * 64);
	XDrawArc(dpy, d, gc, x + w - diameter - 1, y + h - diameter - 1, (unsigned int)diameter, (unsigned int)diameter, 270 * 64, 90 * 64);

	// Draw four lines connecting the arcs - IDENTICAL to draw_rounded_rect
	XDrawLine(dpy, d, gc, x + radius, y, x + w - radius - 1, y);
	XDrawLine(dpy, d, gc, x + w - 1, y + radius, x + w - 1, y + h - radius - 1);
	XDrawLine(dpy, d, gc, x + w - radius - 1, y + h - 1, x + radius, y + h - 1);
	XDrawLine(dpy, d, gc, x, y + h - radius - 1, x, y + radius);
}

/* ========== APPLY ROUNDED CORNER SHAPE TO WINDOW ========== */

static void apply_window_shape(SwatchContext *ctx) {
	if (!ctx) {
		return;
	}
	if (ctx->border_radius <= 0) {
		// If no radius, remove any shape mask to make window rectangular
		if (ctx->border_radius == 0) {
			XShapeCombineMask(ctx->display, ctx->swatch_window, ShapeBounding, 0, 0, None, ShapeSet);
		}
		return;
	}
	XWindowAttributes wa;
	XGetWindowAttributes(ctx->display, ctx->swatch_window, &wa);
	int w = (wa.width > 0) ? wa.width : ctx->swatch_width;
	int h = (wa.height > 0) ? wa.height : ctx->swatch_height;
	if (w <= 0 || h <= 0) {
		return;
	}
	// Create a 1-bit pixmap for the shape mask
	Pixmap mask = XCreatePixmap(ctx->display, ctx->swatch_window, (unsigned int)w, (unsigned int)h, 1);
	if (!mask) {
		return;
	}
	GC mask_gc = XCreateGC(ctx->display, mask, 0, NULL);

	// Clear mask to 0 (invisible everywhere) - like zoom.c does
	XSetForeground(ctx->display, mask_gc, 0);
	XFillRectangle(ctx->display, mask, mask_gc, 0, 0, (unsigned int)w, (unsigned int)h);

	// Draw visible parts with FG=1 - like zoom.c does
	XSetForeground(ctx->display, mask_gc, 1);

	// Fill the rounded rectangle area matching where the border is drawn
	// Border is drawn at (inset, inset) with dimensions (w - border_width, h - border_width)
	int inset = ctx->border_width / 2;
	fill_rounded_rect(ctx->display, mask, mask_gc, inset, inset, w - ctx->border_width, h - ctx->border_width, ctx->border_radius);

	// Apply the shape mask to the window
	XShapeCombineMask(ctx->display, ctx->swatch_window, ShapeBounding, 0, 0, mask, ShapeSet);

	XFreeGC(ctx->display, mask_gc);
	XFreePixmap(ctx->display, mask);
}

/* ========== BORDER DRAWING ========== */

static void swatch_draw_border(SwatchContext *ctx) {
	if (!ctx) {
		return;
	}
	unsigned long border_px;
	if (ctx->attached) {
		border_px = enhanced_border_color(ctx->display, ctx->screen, ctx->last_pixel, ctx->main_bg_pixel);
	}
	else {
		border_px = ctx->last_pixel;
	}
	GC gc = XCreateGC(ctx->display, ctx->swatch_window, 0, NULL);
	XSetForeground(ctx->display, gc, border_px);
	XSetLineAttributes(ctx->display, gc, (unsigned int)ctx->border_width, LineSolid, CapButt, JoinMiter);

	XWindowAttributes wa;
	XGetWindowAttributes(ctx->display, ctx->swatch_window, &wa);
	int w = (wa.width > 0) ? wa.width : ctx->swatch_width;
	int h = (wa.height > 0) ? wa.height : ctx->swatch_height;
	if (w > 0 && h > 0) {
		// Draw border to appropriate target
		Drawable draw_target = ctx->use_dbe ? ctx->dbe_back_buffer : ctx->swatch_window;
		int inset = ctx->border_width / 2;
		
		// Fill background with the swatch color first
		GC fill_gc = XCreateGC(ctx->display, ctx->swatch_window, 0, NULL);
		XSetForeground(ctx->display, fill_gc, ctx->last_pixel);
		fill_rounded_rect(ctx->display, draw_target, fill_gc, inset, inset, w - ctx->border_width, h - ctx->border_width, ctx->border_radius);
		XFreeGC(ctx->display, fill_gc);
		
		// Then draw the border
		draw_rounded_rect(ctx->display, draw_target, gc, inset, inset, w - ctx->border_width, h - ctx->border_width, ctx->border_radius);
		
		// If using DBE, swap buffers to present
		if (ctx->use_dbe) {
			dbe_swap_buffers(ctx->dbe_ctx, ctx->swatch_window, XdbeUndefined);
		}
	}
	XFreeGC(ctx->display, gc);
}

/* ========== PUBLIC API ========== */

/**
 * @brief Create a new color swatch widget
 *
 * See swatch.h for full documentation.
 * Initializes swatch window with configurable dimensions and border mode support.
 */
SwatchContext *swatch_create(Display *dpy, Window parent, int width, int height) {
	if (!dpy) {
		return NULL;
	}
	SwatchContext *ctx = (SwatchContext *)calloc(1, sizeof(SwatchContext));
	if (!ctx) {
		return NULL;
	}
	ctx->display = dpy;
	ctx->screen = DefaultScreen(dpy);
	ctx->parent = parent;
	ctx->swatch_width = width;
	ctx->swatch_height = height;
	ctx->moving = 0;
	ctx->attached = 1;
	ctx->border_width = 2; // Default, will be overridden by config
	ctx->border_radius = 4; // Default, will be overridden by config

	ctx->initial_x = 310;
	ctx->initial_y = 215;
	
	// Initialize DBE context
	ctx->dbe_ctx = dbe_init(dpy, ctx->screen);
	ctx->dbe_back_buffer = None;
	ctx->use_dbe = 0;

	XSetWindowAttributes swa;
	swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
	swa.override_redirect = True;
	swa.background_pixmap = None;

	ctx->swatch_window = XCreateWindow(
		dpy, parent, 0, 0, (unsigned int)width, (unsigned int)height, 0, CopyFromParent, InputOutput, CopyFromParent, CWBackPixmap | CWOverrideRedirect | CWEventMask, &swa);
	if (!ctx->swatch_window) {
		free(ctx);
		return NULL;
	}
	// Apply rounded corner shape to the window
	apply_window_shape(ctx);
	
	// Initialize DBE buffers after window creation
	if (ctx->dbe_ctx && dbe_is_supported(ctx->dbe_ctx)) {
		ctx->dbe_back_buffer = dbe_allocate_back_buffer(ctx->dbe_ctx, ctx->swatch_window, XdbeUndefined);
		ctx->use_dbe = (ctx->dbe_back_buffer != None);
	} else {
		ctx->use_dbe = 0;
	}

	XMapWindow(dpy, ctx->swatch_window);
	
	// Initial draw to show the color
	swatch_draw_border(ctx);
	
	return ctx;
}

/**
 * @brief Destroy swatch widget and free resources
 *
 * See swatch.h for full documentation.
 */
void swatch_destroy(SwatchContext *ctx) {
	if (!ctx) {
		return;
	}
	// Clean up DBE resources
	if (ctx->dbe_back_buffer != None) {
		dbe_deallocate_back_buffer(ctx->dbe_ctx, ctx->dbe_back_buffer);
	}
	if (ctx->dbe_ctx) {
		dbe_destroy(ctx->dbe_ctx);
	}
	if (ctx->swatch_window) {
		XDestroyWindow(ctx->display, ctx->swatch_window);
	}
	free(ctx);
}

Window swatch_get_window(SwatchContext *ctx) {
	return ctx ? ctx->swatch_window : None;
}

/**
 * @brief Process X11 events for swatch
 *
 * See swatch.h for full documentation.
 * Handles ButtonPress and Expose events.
 */
int swatch_handle_event(SwatchContext *ctx, const XEvent *ev, Window main_window) {
	(void)main_window;
	if (!ctx || !ev) {
		return 0;
	}
	Window root, child;
	unsigned int mask;
	if (ev->type == Expose && ev->xexpose.window == ctx->swatch_window) {
		if (ev->xexpose.count == 0) {
			swatch_draw_border(ctx);
		}
		return 1;
	}
	if (ev->type == MotionNotify && ctx->moving) {
		XQueryPointer(ctx->display, DefaultRootWindow(ctx->display), &root, &child, &ctx->root_x, &ctx->root_y, &ctx->swatch_x, &ctx->swatch_y, &mask);
		XMoveWindow(ctx->display, ctx->swatch_window, ctx->swatch_x - ctx->swatch_width / 2, ctx->swatch_y - ctx->swatch_height / 2);
		return 1;
	}
	if (ev->type == ButtonPress && ev->xbutton.button == Button1 && ev->xany.window == ctx->swatch_window) {
		XQueryPointer(ctx->display, DefaultRootWindow(ctx->display), &root, &child, &ctx->root_x, &ctx->root_y, &ctx->swatch_x, &ctx->swatch_y, &mask);
		XUnmapWindow(ctx->display, ctx->swatch_window);
		XReparentWindow(ctx->display, ctx->swatch_window, DefaultRootWindow(ctx->display), ctx->root_x - ctx->swatch_width / 2, ctx->root_y - ctx->swatch_height / 2);
		XMapRaised(ctx->display, ctx->swatch_window);
		ctx->attached = 0;
		cursor_invisible(ctx);
		XGrabPointer(ctx->display, ctx->swatch_window, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, ctx->swatch_window, None, CurrentTime);
		swatch_draw_border(ctx);
		ctx->moving = 1;
		return 1;
	}
	if (ev->type == ButtonRelease && ev->xbutton.button == Button1 && ev->xany.window == ctx->swatch_window) {
		XUnmapWindow(ctx->display, ctx->swatch_window);
		XReparentWindow(ctx->display, ctx->swatch_window, ctx->parent, ctx->initial_x, ctx->initial_y);
		XMapRaised(ctx->display, ctx->swatch_window);
		ctx->attached = 1;
		cursor_normal(ctx);
		XUngrabPointer(ctx->display, CurrentTime);
		ctx->moving = 0;
		swatch_draw_border(ctx);
		return 1;
	}
	return 0;
}

/**
 * @brief Set swatch color
 *
 * See swatch.h for full documentation.
 * Updates color display with automatic contrast-aware border.
 */
void swatch_set_color(SwatchContext *ctx, unsigned long pixel) {
	if (!ctx) {
		return;
	}
	ctx->last_pixel = pixel;

	XSetWindowBackground(ctx->display, ctx->swatch_window, pixel);
	XClearWindow(ctx->display, ctx->swatch_window);

	XEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = Expose;
	ev.xexpose.window = ctx->swatch_window;
	ev.xexpose.count = 0;
	XSendEvent(ctx->display, ctx->swatch_window, False, ExposureMask, &ev);
	XFlush(ctx->display);
}

void swatch_set_background(SwatchContext *ctx, unsigned long bg_pixel) {
	if (!ctx) {
		return;
	}
	ctx->main_bg_pixel = bg_pixel;
	XEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = Expose;
	ev.xexpose.window = ctx->swatch_window;
	ev.xexpose.count = 0;
	XSendEvent(ctx->display, ctx->swatch_window, False, ExposureMask, &ev);
	XFlush(ctx->display);
}

/**
 * @brief Move swatch to new position
 *
 * See swatch.h for full documentation.
 */
void swatch_set_position(SwatchContext *ctx, int x, int y) {
	if (!ctx) {
		return;
	}
	XMoveWindow(ctx->display, ctx->swatch_window, x, y);
	ctx->initial_x = x;
	ctx->initial_y = y;
}

void swatch_set_border(SwatchContext *ctx, int border_width, int border_radius) {
	if (!ctx) {
		return;
	}
	ctx->border_width = border_width;
	ctx->border_radius = border_radius;

	// Update window shape with new radius
	apply_window_shape(ctx);

	// Clear the window to remove old border pixels
	XClearWindow(ctx->display, ctx->swatch_window);

	// Force immediate redraw of border
	swatch_draw_border(ctx);

	XFlush(ctx->display);
}

void swatch_resize(SwatchContext *ctx, int width, int height) {
	if (!ctx || width <= 0 || height <= 0) {
		return;
	}
	ctx->swatch_width = width;
	ctx->swatch_height = height;

	// Resize the window
	XResizeWindow(ctx->display, ctx->swatch_window, (unsigned int)width, (unsigned int)height);

	// Reinitialize DBE buffers for new window size
	if (ctx->dbe_back_buffer != None) {
		dbe_deallocate_back_buffer(ctx->dbe_ctx, ctx->dbe_back_buffer);
		ctx->dbe_back_buffer = None;
	}
	if (ctx->dbe_ctx && dbe_is_supported(ctx->dbe_ctx)) {
		ctx->dbe_back_buffer = dbe_allocate_back_buffer(ctx->dbe_ctx, ctx->swatch_window, XdbeUndefined);
		ctx->use_dbe = (ctx->dbe_back_buffer != None);
	} else {
		ctx->use_dbe = 0;
	}

	// Update window shape with new dimensions
	apply_window_shape(ctx);

	// Draw using double buffering to eliminate flicker
	if (ctx->use_dbe && ctx->dbe_back_buffer != None) {
		// Draw everything to back buffer first, then swap atomically
		unsigned long border_px;
		if (ctx->attached) {
			border_px = enhanced_border_color(ctx->display, ctx->screen, ctx->last_pixel, ctx->main_bg_pixel);
		}
		else {
			border_px = ctx->last_pixel;
		}
		
		// Fill background with the swatch color first
		GC fill_gc = XCreateGC(ctx->display, ctx->swatch_window, 0, NULL);
		XSetForeground(ctx->display, fill_gc, ctx->last_pixel);
		int inset = ctx->border_width / 2;
		fill_rounded_rect(ctx->display, ctx->dbe_back_buffer, fill_gc, inset, inset, 
		                  width - ctx->border_width, height - ctx->border_width, ctx->border_radius);
		XFreeGC(ctx->display, fill_gc);
		
		// Then draw the border
		GC border_gc = XCreateGC(ctx->display, ctx->swatch_window, 0, NULL);
		XSetForeground(ctx->display, border_gc, border_px);
		XSetLineAttributes(ctx->display, border_gc, (unsigned int)ctx->border_width, LineSolid, CapButt, JoinMiter);
		draw_rounded_rect(ctx->display, ctx->dbe_back_buffer, border_gc, inset, inset, 
		                  width - ctx->border_width, height - ctx->border_width, ctx->border_radius);
		XFreeGC(ctx->display, border_gc);
		
		// Swap buffers atomically to present without flicker
		dbe_swap_buffers(ctx->dbe_ctx, ctx->swatch_window, XdbeUndefined);
	} else {
		// Fallback for non-DBE: clear window and draw directly
		XClearWindow(ctx->display, ctx->swatch_window);
		swatch_draw_border(ctx);
	}

	XFlush(ctx->display);
}

/* ========== CONFIGURATION MANAGEMENT ========== */

void swatch_config_init_defaults(Config *cfg) {
	// Border color - GTK light gray
	cfg->swatch.border = (ConfigColor){
		.r = 0xCD / 255.0f,
		.g = 0xC7 / 255.0f,
		.b = 0xC2 / 255.0f,
		.a = 1.0f
	}; // #CDC7C2
}

void swatch_config_parse(Config *cfg, const char *key, const char *value) {
	// Alphabetized styling keys only - color
	if (strcmp(key, "border") == 0) {
		unsigned int r = 0, g = 0, b = 0;
		if (sscanf(value, "#%02x%02x%02x", &r, &g, &b) == 3) {
			cfg->swatch.border.r = (float)r / 255.0f;
			cfg->swatch.border.g = (float)g / 255.0f;
			cfg->swatch.border.b = (float)b / 255.0f;
			cfg->swatch.border.a = 1.0f;
		}
	}
}

void swatch_config_write(FILE *f, const Config *cfg) {
	// Alphabetized styling keys only - color
	fprintf(f, "[swatch]\n");
	fprintf(f, "border = #%02X%02X%02X\n\n",
		(int)(cfg->swatch.border.r * 255),
		(int)(cfg->swatch.border.g * 255),
		(int)(cfg->swatch.border.b * 255));
}

/* ========== SWATCH WIDGET GEOMETRY (swatch-widget section) ========== */

void swatch_widget_config_init_defaults(Config *cfg) {
	if (!cfg) {
		return;
	}
	cfg->swatch_widget.swatch_x = 310;
	cfg->swatch_widget.swatch_y = 215;
	cfg->swatch_widget.width = 74;
	cfg->swatch_widget.height = 74;
	cfg->swatch_widget.border_width = 1;
	cfg->swatch_widget.border_radius = 4;
}

void swatch_widget_config_parse(Config *cfg, const char *key, const char *value) {
	if (!cfg || !key || !value) {
		return;
	}
	
	// Alphabetized geometry keys only
	if (strcmp(key, "border-radius") == 0) {
		cfg->swatch_widget.border_radius = atoi(value);
	}
	else if (strcmp(key, "border-width") == 0) {
		cfg->swatch_widget.border_width = atoi(value);
	}
	else if (strcmp(key, "height") == 0) {
		cfg->swatch_widget.height = atoi(value);
	}
	else if (strcmp(key, "swatch-x") == 0) {
		cfg->swatch_widget.swatch_x = atoi(value);
	}
	else if (strcmp(key, "swatch-y") == 0) {
		cfg->swatch_widget.swatch_y = atoi(value);
	}
	else if (strcmp(key, "width") == 0) {
		cfg->swatch_widget.width = atoi(value);
	}
}

void swatch_widget_config_write(FILE *f, const Config *cfg) {
	if (!f || !cfg) {
		return;
	}
	
	// Alphabetized geometry keys only
	fprintf(f, "[swatch-widget]\n");
	fprintf(f, "border-radius = %d\n", cfg->swatch_widget.border_radius);
	fprintf(f, "border-width = %d\n", cfg->swatch_widget.border_width);
	fprintf(f, "height = %d\n", cfg->swatch_widget.height);
	fprintf(f, "swatch-x = %d\n", cfg->swatch_widget.swatch_x);
	fprintf(f, "swatch-y = %d\n", cfg->swatch_widget.swatch_y);
	fprintf(f, "width = %d\n\n", cfg->swatch_widget.width);
}
