/* button.c - Button Widget Implementation
 *
 * Implements a simple, themeable button widget with hover and press states.
 * Supports rounded borders, configurable colors, and X11 Xft font rendering.
 *
 * Features:
 * - Three visual states: normal, hover, pressed
 * - Rounded rectangle borders with configurable radius
 * - Xft font rendering with antialiasing
 * - Complete event handling (enter/leave/press/release)
 * - Dynamic theme updates without widget recreation
 *
 * Internal design notes:
 * - Rendering path prefers DBE when available; falls back to single buffer.
 * - Colors are cached as pixels to avoid repeated XAllocColor calls.
 * - Theme updates simply refresh cached pixels/fonts without recreating windows.
 */

#include "button.h"
#include "dbe.h"
#include <X11/Xft/Xft.h>
#include <X11/Xatom.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ButtonContext {
	Display *display;
	int screen;
	Window parent;
	Window button_win;

	GC gc;
	XftDraw *draw;
	XftFont *font;
	XftColor xft_fg;

	ButtonBlock style;

	int is_pressed;
	int is_hovering;

	int x;
	int y;
	int width;
	int height;
	
	// Geometry properties
	int padding;
	int border_width;
	int hover_border_width;
	int active_border_width;
	int border_radius;

	// Cached pixels to avoid per-draw allocations
	unsigned long px_bg;
	unsigned long px_border;
	unsigned long px_hover_border;
	unsigned long px_active_border;
	
	// DBE support
	DbeContext *dbe_ctx;
	XdbeBackBuffer dbe_back_buffer;
	int use_dbe; // 1 if DBE is available and initialized
	
	// Label text (dynamically allocated)
	char *label;
};

/* ========== RENDERING HELPERS ========== */
/* Color conversion and font loading now centralized in config.c */

/**
 * @brief Draw a rounded rectangle border
 * @param dpy X11 display
 * @param d Drawable (window or pixmap)
 * @param gc Graphics context
 * @param x X coordinate
 * @param y Y coordinate
 * @param w Width
 * @param h Height
 * @param radius Corner radius in pixels
 *
 * Draws a rounded rectangle border using four corner arcs and four connecting lines.
 * Falls back to regular rectangle if radius is 0 or too large for dimensions.
 * This helper is intentionally duplicated across widgets for portability.
 */
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

/* ========== PUBLIC API ========== */

/**
 * @brief Create a new button widget
 *
 * See button.h for full documentation.
 * Initializes DBE support if available, creates button window, caches colors,
 * loads font, and sets up event handling for all button interactions.
 */
ButtonContext *button_create(Display *dpy, Window parent_window, const ButtonBlock *button_style, int width, int height, int padding, int border_width, int hover_border_width, int active_border_width, int border_radius) {
	ButtonContext *ctx = calloc(1, sizeof(ButtonContext));
	if (!ctx) {
		return NULL;
	}
	ctx->display = dpy;
	ctx->screen = DefaultScreen(dpy);
	ctx->parent = parent_window;
	ctx->style = *button_style;
	ctx->is_pressed = 0;
	ctx->is_hovering = 0;
	ctx->label = NULL; // No label by default - caller must set one
	
	// Initialize DBE context
	ctx->dbe_ctx = dbe_init(dpy, ctx->screen);
	ctx->dbe_back_buffer = None;
	ctx->use_dbe = 0;

	ctx->font = config_open_font(dpy, ctx->screen, button_style->font_family, button_style->font_size);

	// Use provided geometry
	ctx->width = width;
	ctx->height = height;
	ctx->padding = padding;
	ctx->border_width = border_width;
	ctx->hover_border_width = hover_border_width;
	ctx->active_border_width = active_border_width;
	ctx->border_radius = border_radius;

	XSetWindowAttributes attr;
	attr.border_pixel = 0; // no server-side border; we draw our own
	// Cache pixels for colors
	ctx->px_bg = config_color_to_pixel(dpy, ctx->screen, button_style->bg);
	ctx->px_border = config_color_to_pixel(dpy, ctx->screen, button_style->border);
	ctx->px_hover_border = config_color_to_pixel(dpy, ctx->screen, button_style->hover_border);
	ctx->px_active_border = config_color_to_pixel(dpy, ctx->screen, button_style->active_border);

	attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | EnterWindowMask | LeaveWindowMask | PointerMotionMask;

	ctx->button_win = XCreateWindow(dpy, parent_window, 0, 0, (unsigned int)ctx->width, (unsigned int)ctx->height, 0, CopyFromParent, InputOutput, CopyFromParent, CWBorderPixel | CWEventMask, &attr);

	// Initialize DBE buffers after window creation
	if (ctx->dbe_ctx && dbe_is_supported(ctx->dbe_ctx)) {
		ctx->dbe_back_buffer = dbe_allocate_back_buffer(ctx->dbe_ctx, ctx->button_win, XdbeUndefined);
		ctx->use_dbe = (ctx->dbe_back_buffer != None);
	} else {
		ctx->use_dbe = 0;
	}

	ctx->gc = XCreateGC(dpy, ctx->button_win, 0, NULL);

	// Use appropriate drawable for XftDraw
	Drawable draw_target = ctx->use_dbe ? ctx->dbe_back_buffer : ctx->button_win;
	ctx->draw = XftDrawCreate(dpy, draw_target, DefaultVisual(dpy, ctx->screen), DefaultColormap(dpy, ctx->screen));

	XRenderColor xr;
	#define CLAMP_COMP(c) ((c) < 0.0 ? 0 : (c) > 1.0 ? 65535 : (unsigned short)((c) * 65535.0 + 0.5))
	xr.red = CLAMP_COMP(button_style->fg.r);
	xr.green = CLAMP_COMP(button_style->fg.g);
	xr.blue = CLAMP_COMP(button_style->fg.b);
	xr.alpha = CLAMP_COMP(button_style->fg.a);
	#undef CLAMP_COMP
	XftColorAllocValue(dpy, DefaultVisual(dpy, ctx->screen), DefaultColormap(dpy, ctx->screen), &xr, &ctx->xft_fg);

	XMapWindow(dpy, ctx->button_win);
	button_draw(ctx);

	return ctx;
}

/**
 * @brief Destroy button widget and free all resources
 *
 * See button.h for full documentation.
 * Frees Xft resources, DBE buffers, GC, window, and context memory.
 */
void button_destroy(ButtonContext *button_context) {
	if (!button_context) {
		return;
	}
	// Free Xft color
	XftColorFree(button_context->display, DefaultVisual(button_context->display, button_context->screen), DefaultColormap(button_context->display, button_context->screen), &button_context->xft_fg);
	// Clean up DBE resources
	if (button_context->dbe_back_buffer != None) {
		dbe_deallocate_back_buffer(button_context->dbe_ctx, button_context->dbe_back_buffer);
	}
	if (button_context->dbe_ctx) {
		dbe_destroy(button_context->dbe_ctx);
	}
	if (button_context->draw) {
		XftDrawDestroy(button_context->draw);
	}
	if (button_context->font) {
		XftFontClose(button_context->display, button_context->font);
	}
	if (button_context->gc) {
		XFreeGC(button_context->display, button_context->gc);
	}
	if (button_context->button_win) {
		XDestroyWindow(button_context->display, button_context->button_win);
	}
	if (button_context->label) {
		free(button_context->label);
	}
	free(button_context);
}

/**
 * @brief Set button label text
 * @param button_context Button widget context
 * @param label New label text (NULL to clear, will be copied)
 *
 * Sets the button's label text. Pass NULL to clear the label.
 * The label is centered both horizontally and vertically.
 */
void button_set_label(ButtonContext *button_context, const char *label) {
	if (!button_context) {
		return;
	}
	// Free old label
	if (button_context->label) {
		free(button_context->label);
		button_context->label = NULL;
	}
	// Copy new label
	if (label) {
		button_context->label = strdup(label);
	}
	// Redraw with new label
	button_draw(button_context);
}

/**
 * @brief Get button's X11 window handle
 *
 * See button.h for full documentation.
 */
// cppcheck-suppress unusedFunction
Window button_get_window(ButtonContext *button_context) {
	return button_context ? button_context->button_win : None;
}

/**
 * @brief Redraw button widget
 *
 * See button.h for full documentation.
 * Renders background, border (with state-dependent colors/width), and label text.
 * Uses DBE double-buffering if available for smooth updates.
 */
void button_draw(ButtonContext *button_context) {
	if (!button_context) {
		return;
	}
	
	// Determine drawing target
	Drawable draw_target = button_context->use_dbe ? button_context->dbe_back_buffer : button_context->button_win;
	
	XSetForeground(button_context->display, button_context->gc, button_context->px_bg);
	XFillRectangle(button_context->display, draw_target, button_context->gc, 0, 0, (unsigned int)button_context->width, (unsigned int)button_context->height);

	ConfigColor border_color = button_context->style.border;
	int border_width = button_context->border_width;
	if (button_context->is_pressed) {
		border_color = button_context->style.active_border;
		border_width = button_context->active_border_width;
	}
	else if (button_context->is_hovering) {
		border_color = button_context->style.hover_border;
		border_width = button_context->hover_border_width;
	}
	unsigned long border_px = button_context->px_border;
	if (border_color.r == button_context->style.hover_border.r && border_color.g == button_context->style.hover_border.g && border_color.b == button_context->style.hover_border.b && border_color.a == button_context->style.hover_border.a) {
		border_px = button_context->px_hover_border;
	}
	else if (border_color.r == button_context->style.active_border.r && border_color.g == button_context->style.active_border.g && border_color.b == button_context->style.active_border.b && border_color.a == button_context->style.active_border.a) {
		border_px = button_context->px_active_border;
	}
	XSetForeground(button_context->display, button_context->gc, border_px);
	XSetLineAttributes(button_context->display, button_context->gc, (unsigned int)border_width, LineSolid, CapButt, JoinMiter);

	int inset = border_width / 2;
	draw_rounded_rect(button_context->display, draw_target, button_context->gc, inset, inset, button_context->width - border_width, button_context->height - border_width, button_context->border_radius);
	// Draw label text centered if Xft draw is available and label is set
	if (button_context->draw && button_context->font && button_context->label) {
		const char *label = button_context->label;
		int label_len = (int)strlen(label);
		
		// Calculate text width for centering
		XGlyphInfo extents;
		XftTextExtentsUtf8(button_context->display, button_context->font, (const FcChar8 *)label, label_len, &extents);
		
		// Center text horizontally and vertically
		int text_x = (button_context->width - extents.width) / 2;
		int text_y = (button_context->height + button_context->font->ascent - button_context->font->descent) / 2;
		
		XftDrawStringUtf8(button_context->draw, &button_context->xft_fg, button_context->font, text_x, text_y, (const FcChar8 *)label, label_len);
	}
	
	// If using DBE, swap buffers to present
	if (button_context->use_dbe) {
		dbe_swap_buffers(button_context->dbe_ctx, button_context->button_win, XdbeUndefined);
	}
}

/**
 * @brief Process X11 events for button
 *
 * See button.h for full documentation.
 * Handles press/release, hover, expose, and motion events.
 */
int button_handle_event(ButtonContext *button_context, const XEvent *event) {
	if (!button_context) {
		return 0;
	}
	if (event->type == ButtonPress && event->xbutton.button == Button1) {
		if (event->xany.window == button_context->button_win) {
			button_context->is_pressed = 1;
			button_draw(button_context);
			return 1;
		}
	}
	else if (event->type == ButtonRelease && event->xbutton.button == Button1) {
		if (event->xany.window == button_context->button_win) {
			// Return 2 to signal click, but don't reset is_pressed
			// Button stays active until button_reset() is called after color pick
			return 2;
		}
	}
	else if (event->type == Expose) {
		if (event->xany.window == button_context->button_win) {
			button_draw(button_context);
			return 1;
		}
	}
	else if (event->type == EnterNotify) {
		if (event->xany.window == button_context->button_win) {
			button_context->is_hovering = 1;
			button_draw(button_context);
			return 1;
		}
	}
	else if (event->type == LeaveNotify) {
		if (event->xany.window == button_context->button_win) {
			button_context->is_hovering = 0;
			button_draw(button_context);
			return 1;
		}
	}
	else if (event->type == MotionNotify) {
		if (event->xany.window == button_context->button_win) {
			// Maintain hover state while cursor moves within the button
			if (!button_context->is_hovering) {
				button_context->is_hovering = 1;
				button_draw(button_context);
			}
			return 1;
		}
	}
	return 0;
}

/**
 * @brief Reset button to unpressed state
 *
 * See button.h for full documentation.
 */
void button_reset(ButtonContext *button_context) {
	if (button_context) {
		button_context->is_pressed = 0;
		button_draw(button_context);
	}
}

/**
 * @brief Manually set button pressed state
 *
 * See button.h for full documentation.
 */
void button_set_pressed(ButtonContext *button_context, int pressed) {
	if (!button_context) {
		return;
	}
	button_context->is_pressed = pressed ? 1 : 0;
	button_draw(button_context);
}

/**
 * @brief Move button to new position
 *
 * See button.h for full documentation.
 */
void button_set_position(ButtonContext *button_context, int x_pos, int y_pos) {
	if (!button_context) {
		return;
	}
	button_context->x = x_pos;
	button_context->y = y_pos;
	XMoveWindow(button_context->display, button_context->button_win, x_pos, y_pos);
}

/**
 * @brief Apply new theme to button
 *
 * See button.h for full documentation.
 * Updates colors, fonts, and redraws without recreating the widget.
 */
void button_set_theme(ButtonContext *button_context, const ButtonBlock *button_style) {
	if (!button_context || !button_style) {
		return;
	}
	button_context->style = *button_style;
	// Update font
	if (button_context->font) {
		XftFontClose(button_context->display, button_context->font);
		button_context->font = NULL;
	}
	button_context->font = config_open_font(button_context->display, button_context->screen, button_style->font_family, button_style->font_size);
	// Update text color
	XftColorFree(button_context->display, DefaultVisual(button_context->display, button_context->screen), DefaultColormap(button_context->display, button_context->screen), &button_context->xft_fg);
	XRenderColor xr;
	// Convert float [0.0-1.0] to X11 component [0-65535] with clamping
	#define CLAMP_COMP(c) ((c) < 0.0 ? 0 : (c) > 1.0 ? 65535 : (unsigned short)((c) * 65535.0 + 0.5))
	xr.red = CLAMP_COMP(button_style->fg.r);
	xr.green = CLAMP_COMP(button_style->fg.g);
	xr.blue = CLAMP_COMP(button_style->fg.b);
	xr.alpha = CLAMP_COMP(button_style->fg.a);
	#undef CLAMP_COMP
	XftColorAllocValue(button_context->display, DefaultVisual(button_context->display, button_context->screen), DefaultColormap(button_context->display, button_context->screen), &xr, &button_context->xft_fg);
	// Recompute cached pixels and update background
	button_context->px_bg = config_color_to_pixel(button_context->display, button_context->screen, button_style->bg);
	button_context->px_border = config_color_to_pixel(button_context->display, button_context->screen, button_style->border);
	button_context->px_hover_border = config_color_to_pixel(button_context->display, button_context->screen, button_style->hover_border);
	button_context->px_active_border = config_color_to_pixel(button_context->display, button_context->screen, button_style->active_border);
	XSetWindowBackground(button_context->display, button_context->button_win, button_context->px_bg);
	// Resize window to new dimensions
	XResizeWindow(button_context->display, button_context->button_win, (unsigned int)button_context->width, (unsigned int)button_context->height);
	
	// Reinitialize DBE buffers for new window size
	if (button_context->dbe_back_buffer != None) {
		dbe_deallocate_back_buffer(button_context->dbe_ctx, button_context->dbe_back_buffer);
		button_context->dbe_back_buffer = None;
	}
	if (button_context->dbe_ctx && dbe_is_supported(button_context->dbe_ctx)) {
		button_context->dbe_back_buffer = dbe_allocate_back_buffer(button_context->dbe_ctx, button_context->button_win, XdbeUndefined);
		button_context->use_dbe = (button_context->dbe_back_buffer != None);
	} else {
		button_context->use_dbe = 0;
	}
	
	// Recreate XftDraw context with appropriate target
	if (button_context->draw) {
		XftDrawDestroy(button_context->draw);
	}
	Drawable draw_target = button_context->use_dbe ? button_context->dbe_back_buffer : button_context->button_win;
	button_context->draw = XftDrawCreate(button_context->display, draw_target, DefaultVisual(button_context->display, button_context->screen), DefaultColormap(button_context->display, button_context->screen));
	
	// Redraw
	button_draw(button_context);
}

/* ========== CONFIGURATION MANAGEMENT ========== */

void button_config_init_defaults(ButtonBlock *button_cfg) {
	if (!button_cfg) {
		return;
	}
	
	ConfigColor default_fg = (ConfigColor){0.180, 0.204, 0.212, 1.0}; // #2E3436 dark slate
	ConfigColor default_bg = (ConfigColor){1.0, 1.0, 1.0, 1.0}; // #FFFFFF white
	ConfigColor default_border = (ConfigColor){0.804, 0.780, 0.761, 1.0}; // #CDC7C2 light gray
	ConfigColor hover_border = (ConfigColor){0.384, 0.627, 0.918, 1.0}; // #62A0EA light blue
	ConfigColor active_border = (ConfigColor){0.110, 0.443, 0.847, 1.0}; // #1C71D8 accent blue

	strncpy(button_cfg->font_family, "DejaVu Sans", sizeof(button_cfg->font_family) - 1);
	button_cfg->font_family[sizeof(button_cfg->font_family) - 1] = '\0';
	button_cfg->font_size = 14;
	button_cfg->fg = default_fg;
	button_cfg->bg = default_bg;
	button_cfg->border = default_border;
	button_cfg->hover_border = hover_border;
	button_cfg->active_border = active_border;
}

int button_config_parse(ButtonBlock *button_cfg, const char *key, const char *value) {
	if (!button_cfg || !key || !value) {
		return 0;
	}
	
	// Alphabetized styling keys only
	if (strcmp(key, "active-border") == 0) {
		button_cfg->active_border = parse_color(value);
		return 1;
	}
	else if (strcmp(key, "background") == 0) {
		button_cfg->bg = parse_color(value);
		return 1;
	}
	else if (strcmp(key, "border") == 0) {
		button_cfg->border = parse_color(value);
		return 1;
	}
	else if (strcmp(key, "color") == 0) {
		button_cfg->fg = parse_color(value);
		return 1;
	}
	else if (strcmp(key, "font") == 0 || strcmp(key, "font-family") == 0) {
		strncpy(button_cfg->font_family, value, sizeof(button_cfg->font_family) - 1);
		button_cfg->font_family[sizeof(button_cfg->font_family) - 1] = '\0';
		return 1;
	}
	else if (strcmp(key, "font-size") == 0) {
		button_cfg->font_size = atoi(value);
		return 1;
	}
	else if (strcmp(key, "hover-border") == 0) {
		button_cfg->hover_border = parse_color(value);
		return 1;
	}
	
	return 0; // Key not recognized
}

int button_config_write(FILE *f, const ButtonBlock *button_cfg) {
	if (!f || !button_cfg) {
		return -1;
	}
	
	// Alphabetized styling keys only - colors and fonts
	fprintf(f, "[button]\n");
	fprintf(f, "active-border = #%02X%02X%02X\n", 
		(int)(button_cfg->active_border.r * 255), 
		(int)(button_cfg->active_border.g * 255), 
		(int)(button_cfg->active_border.b * 255));
	fprintf(f, "background = #%02X%02X%02X\n", 
		(int)(button_cfg->bg.r * 255), 
		(int)(button_cfg->bg.g * 255), 
		(int)(button_cfg->bg.b * 255));
	fprintf(f, "border = #%02X%02X%02X\n", 
		(int)(button_cfg->border.r * 255), 
		(int)(button_cfg->border.g * 255), 
		(int)(button_cfg->border.b * 255));
	fprintf(f, "color = #%02X%02X%02X\n", 
		(int)(button_cfg->fg.r * 255), 
		(int)(button_cfg->fg.g * 255), 
		(int)(button_cfg->fg.b * 255));
	fprintf(f, "font-family = %s\n", button_cfg->font_family);
	fprintf(f, "font-size = %d\n", button_cfg->font_size);
	fprintf(f, "hover-border = #%02X%02X%02X\n\n", 
		(int)(button_cfg->hover_border.r * 255), 
		(int)(button_cfg->hover_border.g * 255), 
		(int)(button_cfg->hover_border.b * 255));
	
	return 0;
}

/* ========== BUTTON WIDGET GEOMETRY (button-widget section) ========== */

void button_widget_config_init_defaults(Config *cfg) {
	if (!cfg) {
		return;
	}
	cfg->button_widget.button_x = 492;
	cfg->button_widget.button_y = 255;
	cfg->button_widget.width = 88;
	cfg->button_widget.height = 32;
	cfg->button_widget.padding = 8;
	cfg->button_widget.border_width = 1;
	cfg->button_widget.hover_border_width = 1;
	cfg->button_widget.active_border_width = 1;
	cfg->button_widget.border_radius = 4;
}

int button_widget_config_parse(Config *cfg, const char *key, const char *value) {
	if (!cfg || !key || !value) {
		return 0;
	}
	
	// Alphabetized geometry keys only
	if (strcmp(key, "active-border-width") == 0) {
		cfg->button_widget.active_border_width = atoi(value);
		return 1;
	}
	else if (strcmp(key, "border-radius") == 0) {
		cfg->button_widget.border_radius = atoi(value);
		return 1;
	}
	else if (strcmp(key, "border-width") == 0) {
		cfg->button_widget.border_width = atoi(value);
		return 1;
	}
	else if (strcmp(key, "button-x") == 0) {
		cfg->button_widget.button_x = atoi(value);
		return 1;
	}
	else if (strcmp(key, "button-y") == 0) {
		cfg->button_widget.button_y = atoi(value);
		return 1;
	}
	else if (strcmp(key, "height") == 0) {
		cfg->button_widget.height = atoi(value);
		return 1;
	}
	else if (strcmp(key, "hover-border-width") == 0) {
		cfg->button_widget.hover_border_width = atoi(value);
		return 1;
	}
	else if (strcmp(key, "padding") == 0) {
		cfg->button_widget.padding = atoi(value);
		return 1;
	}
	else if (strcmp(key, "width") == 0) {
		cfg->button_widget.width = atoi(value);
		return 1;
	}
	
	return 0; // Key not recognized
}

int button_widget_config_write(FILE *f, const Config *cfg) {
	if (!f || !cfg) {
		return -1;
	}
	
	// Alphabetized geometry keys only
	fprintf(f, "[button-widget]\n");
	fprintf(f, "active-border-width = %d\n", cfg->button_widget.active_border_width);
	fprintf(f, "border-radius = %d\n", cfg->button_widget.border_radius);
	fprintf(f, "border-width = %d\n", cfg->button_widget.border_width);
	fprintf(f, "button-x = %d\n", cfg->button_widget.button_x);
	fprintf(f, "button-y = %d\n", cfg->button_widget.button_y);
	fprintf(f, "height = %d\n", cfg->button_widget.height);
	fprintf(f, "hover-border-width = %d\n", cfg->button_widget.hover_border_width);
	fprintf(f, "padding = %d\n", cfg->button_widget.padding);
	fprintf(f, "width = %d\n\n", cfg->button_widget.width);
	
	return 0;
}
