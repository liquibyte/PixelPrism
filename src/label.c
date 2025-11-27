/* label.c - Label Widget Implementation
 *
 * Implements a simple label widget with configurable font, colors, and borders.
 * Supports optional border rendering and automatic text truncation with ellipsis
 * when the label exceeds available width.
 *
 * Internal design notes:
 * - Labels are lightweight: a single Window with optional border drawing.
 * - Text truncation uses Xft glyph extents to avoid partial characters.
 * - Theme updates simply replace cached colors and fonts.
 *
 * Features:
 * - Automatic width/height calculation from text and font
 * - Manual size override support
 * - Xft font rendering with antialiasing
 * - Themeable colors and styling
 * - Efficient redraw on text changes
 */

#include "label.h"
#include "dbe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <math.h>

/* ========== COLOR HELPERS ========== */
typedef struct {
	double r, g, b, a;
} RGBA;

static unsigned short comp(double v) {
	int iv = (int)(v * 65535.0);
	if (iv < 0) iv = 0;
	if (iv > 65535) iv = 65535;
	return (unsigned short)iv;
}

static XftColor xft_from_rgba(Display *dpy, int screen, RGBA c) {
	XftColor x;
	XRenderColor xr;
	xr.red = comp(c.r);
	xr.green = comp(c.g);
	xr.blue = comp(c.b);
	xr.alpha = comp(c.a);
	XftColorAllocValue(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &xr, &x);
	return x;
}

static unsigned long to_px(Display *dpy, int screen, RGBA c) {
	XColor x;
	x.red = (unsigned short)(c.r * 65535);
	x.green = (unsigned short)(c.g * 65535);
	x.blue = (unsigned short)(c.b * 65535);
	x.flags = DoRed | DoGreen | DoBlue;
	XAllocColor(dpy, DefaultColormap(dpy, screen), &x);
	return x.pixel;
}

/* ========== FONT & BUFFER MANAGEMENT ========== */

/* Font loading function that respects size */
static XftFont *open_font(Display *dpy, int screen, const char *fam, int size) {
	FcPattern *pat = FcNameParse((const FcChar8 *)(fam && fam[0] ? fam : "sans"));
	if (!pat) {
		return XftFontOpenName(dpy, screen, "sans-14");
	}
	FcPatternAddInteger(pat, FC_PIXEL_SIZE, size > 0 ? size : 14);
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	FcResult res;
	FcPattern *match = XftFontMatch(dpy, screen, pat, &res);
	FcPatternDestroy(pat);
	XftFont *f = NULL;
	if (match) {
		f = XftFontOpenPattern(dpy, match);
		if (!f) {
			// XftFontOpenPattern takes ownership only on success
			FcPatternDestroy(match);
		}
	}
	if (!f) {
		f = XftFontOpenName(dpy, screen, "sans-14");
	}
	return f;
}

/* Label widget internal structure */
struct LabelContext {
	Display *dpy;
	int screen;
	Window parent;
	Window win;

	// Theme and appearance
	BaseTheme theme;
	char *text;

	// Fonts and colors
	XftFont *font;
	XftColor xft_color;
	XftDraw *draw;

	// Geometry
	int x, y;
	int width, height;
	int text_width, text_height;
	
	// Geometry properties (passed at creation)
	int padding;
	int border_width;
	int border_radius;
	int border_enabled;

	// State
	int visible;
	int color_allocated; // Track if xft_color has been allocated
	int needs_redraw;     // Hint flag set by mutating APIs; Expose handler clears it
	
	// DBE support
	DbeContext *dbe_ctx;
	XdbeBackBuffer dbe_back_buffer;
	int use_dbe; // 1 if DBE is available and initialized
};

/* Forward declarations */
static void label_redraw(LabelContext *label);
void label_draw(LabelContext *label);

/* Calculate text dimensions */
static void calculate_text_size(LabelContext *label) {
	// Check for NULL label first before dereferencing
	if (!label) {
		return;
	}
	// If text or font not available, set dimensions to 0
	if (!label->text || !label->font) {
		label->text_width = 0;
		label->text_height = 0;
		return;
	}
	XGlyphInfo extents;
	XftTextExtentsUtf8(label->dpy, label->font, (const FcChar8 *)label->text, (int)strlen(label->text), &extents);

	label->text_width = extents.width;
	label->text_height = extents.height;
}

/* Initialize DBE buffers for label - recreate XftDraw atomically like entry boxes */
static void init_label_buffers(LabelContext *label) {
	if (!label) {
		return;
	}
	
	// CRITICAL: Destroy XftDraw BEFORE deallocating the buffer it points to
	if (label->draw) {
		XftDrawDestroy(label->draw);
		label->draw = NULL;
	}
	
	// Now safe to deallocate the buffer
	if (label->dbe_back_buffer != None) {
		dbe_deallocate_back_buffer(label->dbe_ctx, label->dbe_back_buffer);
		label->dbe_back_buffer = None;
	}
	
	// Initialize DBE if available
	if (label->dbe_ctx && dbe_is_supported(label->dbe_ctx)) {
		label->dbe_back_buffer = dbe_allocate_back_buffer(label->dbe_ctx, label->win, XdbeUndefined);
		label->use_dbe = (label->dbe_back_buffer != None);
	} else {
		label->use_dbe = 0;
	}
	
	Drawable draw_target = label->use_dbe ? label->dbe_back_buffer : label->win;
	label->draw = XftDrawCreate(label->dpy, draw_target, DefaultVisual(label->dpy, label->screen), DefaultColormap(label->dpy, label->screen));
}

/* Update font and colors; recreate DBE/Xft and cache color; mark for redraw.
 * Returns 1 if window was resized, 0 otherwise. */
static int update_appearance(LabelContext *label) {
	if (!label) {
		return 0;
	}
	// Free old font
	if (label->font) {
		XftFontClose(label->dpy, label->font);
		label->font = NULL;
	}
	
	// Load new font with size
	label->font = open_font(label->dpy, label->screen, label->theme.font_family[0] ? label->theme.font_family : "sans", label->theme.font_size);
	if (!label->font) {
		label->font = open_font(label->dpy, label->screen, "sans", 14);
	}
	
	// Auto-calculate height based on font (same as entry boxes)
	int pad = label->padding;
	int new_h = label->font->ascent + label->font->descent + pad * 2 + 2;
	if (new_h < 22) {
		new_h = 22;
	}
	int did_resize = 0;
	if (new_h != label->height) {
		label->height = new_h;
		if (label->win) {
			XResizeWindow(label->dpy, label->win, (unsigned int)label->width, (unsigned int)label->height);
			did_resize = 1;
		}
	}
	
	// Reinitialize DBE buffers - XftDraw recreated atomically inside like entry boxes
	if (label->win) {
		init_label_buffers(label);
	}
	
	// Cache colors like entry boxes - just overwrite, no free
	if (label->draw) {
		RGBA rgba_tmp = {label->theme.fg_r, label->theme.fg_g, label->theme.fg_b, label->theme.fg_a};
		label->xft_color = xft_from_rgba(label->dpy, label->screen, rgba_tmp);
		label->color_allocated = 1;
	}
	// Recalculate text size
	calculate_text_size(label);
	
	// Mark for redraw; caller decides whether Expose will come from resize
	label->needs_redraw = 1;
	
	// Return whether we resized (caller needs to know)
	return did_resize;
}

/* Create label window */
static void create_window(LabelContext *label) {
	if (!label) {
		return;
	}
	XSetWindowAttributes attrs = {
		0
	};
	unsigned long attrmask = CWEventMask;

	attrs.event_mask = ExposureMask;
	attrmask |= CWBackPixmap;

	// Use configured width/height directly
	int w = label->width;
	int h = label->height;

	// Only auto-size if explicitly set to 0 (for backwards compatibility)
	if (w == 0) {
		w = label->text_width + (label->padding * 2);
	}
	if (h == 0) {
		h = label->text_height + (label->padding * 2);
	}
	label->win = XCreateWindow(label->dpy, label->parent, label->x, label->y, (unsigned int)w, (unsigned int)h, 0, CopyFromParent, InputOutput, CopyFromParent, attrmask, &attrs);

	// Store label context in window for later retrieval
	XSaveContext(label->dpy, label->win, 1, (XPointer)label);

	// Ensure first Expose will trigger a draw
	label->needs_redraw = 1;
	// Map the window to make it visible
	XMapWindow(label->dpy, label->win);
}

/* ========== PUBLIC API ========== */

/**
 * @brief Create a new label widget
 *
 * See label.h for full documentation.
 * Initializes label window with text, font, colors, and optional DBE support.
 */
LabelContext *label_create(Display *dpy, int screen, Window parent, int x, int y, int width, int padding, int border_width, int border_radius, int border_enabled, const char *text, const BaseTheme *theme) {
	if (!dpy || !theme) {
		return NULL;
	}
	LabelContext *label = calloc(1, sizeof(LabelContext));
	if (!label) {
		return NULL;
	}
	// Initialize basic properties
	label->dpy = dpy;
	label->screen = screen;
	label->parent = parent;
	label->x = x;
	label->y = y;
	label->width = width;
	label->height = 0; // Will be auto-calculated
	label->padding = padding;
	label->border_width = border_width;
	label->border_radius = border_radius;
	label->border_enabled = border_enabled;
	label->visible = 1; // Start visible

	// Copy theme
	label->theme = *theme;
	// Copy text
	if (text) {
		label->text = strdup(text);
	}
	
	// Initialize DBE context
	label->dbe_ctx = dbe_init(dpy, screen);
	label->dbe_back_buffer = None;
	label->use_dbe = 0;
	
	// Load font FIRST for text measurement
	label->font = open_font(label->dpy, label->screen, label->theme.font_family[0] ? label->theme.font_family : "sans", label->theme.font_size);
	if (!label->font) {
		label->font = open_font(label->dpy, label->screen, "sans", 14);
	}
	// Calculate text size for auto-sizing
	calculate_text_size(label);

	// Create window with correct size
	create_window(label);

	// THEN update appearance (now we have a window)
	update_appearance(label);
	if (!label->win || !label->font || !label->draw) {
		label_destroy(label);
		return NULL;
	}
	return label;
}

/**
 * @brief Destroy label widget and free resources
 *
 * See label.h for full documentation.
 */
void label_destroy(LabelContext *label) {
	if (!label) {
		return;
	}
	// Free resources
	if (label->text) {
		free(label->text);
	}
	// Clean up DBE resources
	if (label->dbe_back_buffer != None) {
		dbe_deallocate_back_buffer(label->dbe_ctx, label->dbe_back_buffer);
	}
	if (label->dbe_ctx) {
		dbe_destroy(label->dbe_ctx);
	}
	// Free Xft resources in correct order per X11/Xft best practices:
	// 1. XftColorFree FIRST (before window destruction)
	if (label->color_allocated) {
		XftColorFree(label->dpy, DefaultVisual(label->dpy, label->screen), DefaultColormap(label->dpy, label->screen), &label->xft_color);
	}
	// 2. XftDrawDestroy
	if (label->draw) {
		XftDrawDestroy(label->draw);
	}
	// 3. XftFontClose
	if (label->font) {
		XftFontClose(label->dpy, label->font);
	}
	// 4. XDestroyWindow LAST
	if (label->win) {
		XDestroyWindow(label->dpy, label->win);
	}
	free(label);
}

/* Theme change draw policy:
 * - Call update_appearance() which may trigger XResizeWindow.
 * - Set needs_redraw.
 * - If resized: rely on Expose from XResizeWindow (handler draws once).
 * - If not resized: post one synthetic Expose so the handler draws once. */
/**
 * @brief Set label text and auto-resize
 *
 * See label.h for full documentation.
 * Updates text, recalculates dimensions, and triggers redraw.
 */
// cppcheck-suppress unusedFunction
void label_set_text(LabelContext *label, const char *text) {
	if (!label) {
		return;
	}
	if (label->text) {
		free(label->text);
		label->text = NULL;
	}
	if (text) {
		label->text = strdup(text);
	}
	calculate_text_size(label);
	// Auto-resize if needed
	if (label->width == 0 || label->height == 0) {
		int w = label->width;
		int h = label->height;
		if (w == 0) {
			w = label->text_width + 8;
		}
		if (h == 0) {
			h = label->text_height + 4;
		}
		XResizeWindow(label->dpy, label->win, (unsigned int)w, (unsigned int)h);
		label->width = w;
		label->height = h;
	}
	// Mark for redraw
	label->needs_redraw = 1;
	// Request Expose
	if (label->visible) {
		XEvent ev = {
			0
		};
		ev.type = Expose;
		ev.xexpose.window = label->win;
		ev.xexpose.count = 0;
		XSendEvent(label->dpy, label->win, False, ExposureMask, &ev);
	}
}

/**
 * @brief Update label theme
 *
 * See label.h for full documentation.
 */
void label_set_theme(LabelContext *label, const BaseTheme *theme) {
	if (!label || !theme) {
		return;
	}
	label->theme = *theme;
	int did_resize = update_appearance(label);

	// Update background color with theme color
	XSetWindowAttributes attrs;
	attrs.background_pixel = to_px(label->dpy, label->screen, (RGBA) { theme->bg_r, theme->bg_g, theme->bg_b, theme->bg_a });
	XChangeWindowAttributes(label->dpy, label->win, CWBackPixel, &attrs);
	
    // Expose-only drawing to avoid double renders
    label->needs_redraw = 1;
    if (label->visible && !did_resize) {
        XEvent ev = {0};
        ev.type = Expose;
        ev.xexpose.window = label->win;
        ev.xexpose.count = 0;
        XSendEvent(label->dpy, label->win, False, ExposureMask, &ev);
    }
}

/**
 * @brief Move label to new position
 *
 * See label.h for full documentation.
 */
void label_move(LabelContext *label, int x, int y) {
    if (!label) {
        return;
    }
    if (label->x == x && label->y == y) {
        return; // No move, avoid extra Expose
    }
    label->x = x;
    label->y = y;
    XMoveWindow(label->dpy, label->win, x, y);
}

void label_resize(LabelContext *label, int width, int height) {
    if (!label) {
        return;
    }
    int w = width;
    int h = height;

	    // Calculate minimum required size for text (match update_appearance)
    int min_width = label->text_width + (label->padding * 2);
    int min_height = (label->font ? (label->font->ascent + label->font->descent) : label->text_height) + (label->padding * 2) + 2;
	// Ensure width is at least minimum required for text
	if (w == 0) {
		w = min_width;
	}
	else if (w < min_width) {
		w = min_width;
	}
	// Ensure height is at least minimum required for text
	if (h == 0) {
		h = min_height;
	}
	else if (h < min_height) {
		h = min_height;
	}
	    
    // Only resize if size actually changed to avoid redundant Expose
    if (w != label->width || h != label->height) {
        XResizeWindow(label->dpy, label->win, (unsigned int)w, (unsigned int)h);
        label->width = w;
        label->height = h;

        // Reinitialize DBE buffers - this handles XftDraw destroy/create internally
        init_label_buffers(label);

        // Cache colors like entry boxes - just overwrite, no free
        if (label->draw) {
            RGBA rgba_tmp = {label->theme.fg_r, label->theme.fg_g, label->theme.fg_b, label->theme.fg_a};
            label->xft_color = xft_from_rgba(label->dpy, label->screen, rgba_tmp);
            label->color_allocated = 1;
        }
        // Mark for redraw
        label->needs_redraw = 1;
    }
}

void label_set_geometry(LabelContext *label, int padding, int border_width, int border_radius, int border_enabled) {
	if (!label) {
		return;
	}
	
	// Update geometry fields
	int changed = 0;
	if (label->padding != padding) {
		label->padding = padding;
		changed = 1;
	}
	if (label->border_width != border_width) {
		label->border_width = border_width;
		changed = 1;
	}
	if (label->border_radius != border_radius) {
		label->border_radius = border_radius;
		changed = 1;
	}
	if (label->border_enabled != border_enabled) {
		label->border_enabled = border_enabled;
		changed = 1;
	}
	
	// If geometry changed, recalculate size and trigger redraw
	if (changed) {
		// Recalculate height based on new padding
		if (label->font) {
			int new_h = label->font->ascent + label->font->descent + label->padding * 2 + 2;
			if (new_h < 22) {
				new_h = 22;
			}
			if (new_h != label->height) {
				label->height = new_h;
				XResizeWindow(label->dpy, label->win, (unsigned int)label->width, (unsigned int)label->height);
				init_label_buffers(label);
			}
		}
		// Mark for redraw to show new borders/padding
		label->needs_redraw = 1;
	}
}

Window label_get_window(LabelContext *label) {
	return label ? label->win : None;
}

// cppcheck-suppress unusedFunction
void label_show(LabelContext *label) {
	if (!label || !label->win) {
		return;
	}
	XMapWindow(label->dpy, label->win);
	label->visible = 1;
	XFlush(label->dpy); // Ensure the window is mapped immediately
}
// cppcheck-suppress unusedFunction
void label_hide(LabelContext *label) {
	if (!label) {
		return;
	}
	if (label->visible) {
		XUnmapWindow(label->dpy, label->win);
		label->visible = 0;
	}
}

/* ========== RENDERING ========== */

/* Internal draw function - like entry redraw():
 * Draw everything to the back buffer in this order and swap once:
 * 1) clear/fill background
 * 2) draw text
 * 3) draw border
 * 4) DBE swap */
static void label_redraw(LabelContext *label) {
	if (!label || !label->draw || !label->font) {
		return;
	}
	
	// Determine drawing target
	Drawable draw_target = label->use_dbe ? label->dbe_back_buffer : label->win;
	
	// Clear and fill background
	if (label->use_dbe) {
		// For DBE, clear only the back buffer using XFillRectangle
		GC clear_gc = XCreateGC(label->dpy, label->win, 0, NULL);
		XSetForeground(label->dpy, clear_gc, to_px(label->dpy, label->screen, (RGBA) { label->theme.bg_r, label->theme.bg_g,
			                                                           label->theme.bg_b, label->theme.bg_a }));
		XFillRectangle(label->dpy, draw_target, clear_gc, 0, 0, (unsigned int)label->width, (unsigned int)label->height);
		XFreeGC(label->dpy, clear_gc);
	} else {
		// For non-DBE, work directly with window
		GC clear_gc = XCreateGC(label->dpy, label->win, 0, NULL);
		XSetForeground(label->dpy, clear_gc, to_px(label->dpy, label->screen, (RGBA) { label->theme.bg_r, label->theme.bg_g,
			                                                           label->theme.bg_b, label->theme.bg_a }));
		XFillRectangle(label->dpy, label->win, clear_gc, 0, 0, (unsigned int)label->width, (unsigned int)label->height);
		XFreeGC(label->dpy, clear_gc);
	}
	
	if (label->text && label->font) {
		// Centered baseline calculation (same as entry boxes for vertical alignment)
		int pad = label->padding;
		int text_h = label->font->ascent + label->font->descent;
		int extra = label->height - (text_h + pad * 2);
		if (extra < 0) {
			extra = 0;
		}
		int baseline = pad + (extra / 2) + label->font->ascent;
		
		// Horizontal text position (same as entry boxes: pad + 2)
		int text_x = pad + 2;

		// Draw text to back buffer
		XftDrawStringUtf8(label->draw, &label->xft_color, label->font, text_x, baseline, (const FcChar8 *)label->text, (int)strlen(label->text));
	}
	
	// Draw border to back buffer BEFORE swap - everything drawn to buffer first
	if (label->border_enabled && label->border_width > 0) {
		GC border_gc = XCreateGC(label->dpy, label->win, 0, NULL);
		XSetForeground(label->dpy, border_gc, to_px(label->dpy, label->screen, (RGBA) { label->theme.border_r, label->theme.border_g,
		                                                           label->theme.border_b, label->theme.border_a }));
		XSetLineAttributes(label->dpy, border_gc, (unsigned int)label->border_width, LineSolid, CapButt, JoinMiter);
		
		// Calculate border inset (half of border width for centered border)
		int inset = label->border_width / 2;
		int border_width = label->width - label->border_width;
		int border_height = label->height - label->border_width;
		
		if (label->border_radius > 0) {
			// Draw rounded rectangle border to BUFFER
			int radius = label->border_radius;
			int diameter = radius * 2;
			
			// Top horizontal line
			XDrawLine(label->dpy, draw_target, border_gc, inset + radius, inset, inset + border_width - radius, inset);
			// Bottom horizontal line
			XDrawLine(label->dpy, draw_target, border_gc, inset + radius, inset + border_height, inset + border_width - radius, inset + border_height);
			// Left vertical line
			XDrawLine(label->dpy, draw_target, border_gc, inset, inset + radius, inset, inset + border_height - radius);
			// Right vertical line
			XDrawLine(label->dpy, draw_target, border_gc, inset + border_width, inset + radius, inset + border_width, inset + border_height - radius);
			
			// Corner arcs
			XDrawArc(label->dpy, draw_target, border_gc, inset, inset, (unsigned int)diameter, (unsigned int)diameter, 90 * 64, 90 * 64);
			XDrawArc(label->dpy, draw_target, border_gc, inset + border_width - diameter, inset, (unsigned int)diameter, (unsigned int)diameter, 0, 90 * 64);
			XDrawArc(label->dpy, draw_target, border_gc, inset, inset + border_height - diameter, (unsigned int)diameter, (unsigned int)diameter, 180 * 64, 90 * 64);
			XDrawArc(label->dpy, draw_target, border_gc, inset + border_width - diameter, inset + border_height - diameter, (unsigned int)diameter, (unsigned int)diameter, 270 * 64, 90 * 64);
		} else {
			// Draw simple rectangle border to BUFFER
			XDrawRectangle(label->dpy, draw_target, border_gc, inset, inset, (unsigned int)border_width, (unsigned int)border_height);
		}
		
		XFreeGC(label->dpy, border_gc);
	}
	
	// NOW swap buffers to present everything at once
	if (label->use_dbe) {
		dbe_swap_buffers(label->dbe_ctx, label->win, XdbeUndefined);
	}
	
	XFlush(label->dpy);
}

/* Public draw function - like entry_draw() */
// cppcheck-suppress unusedFunction
void label_draw(LabelContext *label) {
	if (!label) {
		return;
	}
	label_redraw(label);
}

/* Expose handler (like entry boxes):
 * Always repaints on the final Expose (ev->count == 0) to ensure content is
 * restored after remap/minimize; clears needs_redraw for compatibility. */
int label_handle_expose(LabelContext *label, const XExposeEvent *ev) {
    if (!label) {
        return 0;
    }
    if (ev && ev->count > 0) {
        return 1; // More expose events coming
    }
    label_redraw(label);
    label->needs_redraw = 0;
    return 1;
}

/* ========== CONFIGURATION MANAGEMENT ========== */

/* Forward declaration of color parser from config.c */
extern ConfigColor parse_color(const char *hex_str);

void label_config_init_defaults(LabelConfig *label_cfg, ConfigColor default_fg,
                                 ConfigColor default_bg, ConfigColor default_border) {
	if (!label_cfg) {
		return;
	}
	
	strncpy(label_cfg->font_family, "DejaVu Sans", sizeof(label_cfg->font_family) - 1);
	label_cfg->font_family[sizeof(label_cfg->font_family) - 1] = '\0';
	label_cfg->font_size = 16;
	label_cfg->fg = default_fg;
	label_cfg->bg = default_bg;
	label_cfg->border = default_border;
	
	strncpy(label_cfg->default_font_family, "sans", sizeof(label_cfg->default_font_family) - 1);
	label_cfg->default_font_family[sizeof(label_cfg->default_font_family) - 1] = '\0';
	label_cfg->default_font_size = 14;
}

int label_config_parse(LabelConfig *label_cfg, const char *key, const char *value) {
	if (!label_cfg || !key || !value) {
		return 0;
	}
	
	if (strcmp(key, "font") == 0 || strcmp(key, "font-family") == 0) {
		strncpy(label_cfg->font_family, value, sizeof(label_cfg->font_family) - 1);
		label_cfg->font_family[sizeof(label_cfg->font_family) - 1] = '\0';
		return 1;
	}
	else if (strcmp(key, "font-size") == 0) {
		label_cfg->font_size = atoi(value);
		return 1;
	}
	else if (strcmp(key, "color") == 0) {
		label_cfg->fg = parse_color(value);
		return 1;
	}
	else if (strcmp(key, "background") == 0) {
		label_cfg->bg = parse_color(value);
		return 1;
	}
	else if (strcmp(key, "border") == 0) {
		label_cfg->border = parse_color(value);
		return 1;
	}
	
	return 0; // Key not recognized
}

int label_config_write(FILE *f, const LabelConfig *label_cfg) {
	if (!f || !label_cfg) {
		return -1;
	}
	
	fprintf(f, "[label]\n");
	fprintf(f, "background = #%02X%02X%02X\n", 
		(int)(label_cfg->bg.r * 255), 
		(int)(label_cfg->bg.g * 255), 
		(int)(label_cfg->bg.b * 255));
	fprintf(f, "border = #%02X%02X%02X\n", 
		(int)(label_cfg->border.r * 255), 
		(int)(label_cfg->border.g * 255), 
		(int)(label_cfg->border.b * 255));
	fprintf(f, "color = #%02X%02X%02X\n", 
		(int)(label_cfg->fg.r * 255), 
		(int)(label_cfg->fg.g * 255), 
		(int)(label_cfg->fg.b * 255));
	fprintf(f, "font = %s\n", label_cfg->font_family);
	fprintf(f, "font-size = %d\n\n", label_cfg->font_size);
	
	return 0;
}

// cppcheck-suppress unusedFunction
int label_is_using_dbe(const LabelContext *label) {
	if (!label) {
		return 0;
	}
	return label->use_dbe;
}
