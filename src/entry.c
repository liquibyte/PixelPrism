/* entry.c - Text Entry Widget Implementation
 *
 * Implements a text entry widget with validation, selection, and editing capabilities.
 * Supports multiple entry types (text, integer, float, hex) with context menu integration.
 *
 * Features:
 * - Multiple validation modes (text, integer, float, hex)
 * - Text selection and editing (cut, copy, paste)
 * - Undo/redo functionality
 * - Cursor positioning and blinking
 * - Context menu integration
 * - Keyboard navigation and shortcuts
 * - Themeable appearance with validation states
 *
 * This widget provides comprehensive text input functionality similar to
 * GTK+ entry widgets but with minimal X11 dependencies.
 *
 * Internal design notes:
 * - Text is stored as UTF-8; selection indices are byte offsets.
 * - Undo/redo uses a ring buffer of snapshots rather than incremental deltas.
 * - Cursor blink timing is driven by gettimeofday to avoid global timers.
 */

#include "entry.h"
#include "context.h"
#include "config.h"
#include "dbe.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>

#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>

/* forward from header */
void entry_draw(struct MiniEntry *e);

/* ========== SAFE HELPERS ========== */
static void *safe_realloc(void *p, size_t n) {
	void *q = p ? realloc(p, n) : malloc(n);
	if (!q) {
		fprintf(stderr, "OOM\n");
		abort();
	}
	return q;
}

static char *safe_strdup(const char *s) {
	const char *src = s ? s : "";
	size_t n = strlen(src) + 1;
	char *r = (char *)malloc(n);
	if (!r) {
		perror("malloc");
		abort();
	}
	memcpy(r, src, n);
	return r;
}

/* ========== UTILITY FUNCTIONS ========== */
static char *safe_strndup(const char *s, size_t len) {
	const char *src = s ? s : "";
	size_t n = strlen(src);
	if (n > len) {
		n = len;
	}
	char *r = (char *)malloc(n + 1);
	if (!r) {
		perror("malloc");
		abort();
	}
	memcpy(r, src, n);
	r[n] = 0;
	return r;
}

static unsigned short comp(double c) {
	if (c < 0) {
		c = 0;
	}
	if (c > 1) {
		c = 1;
	}
	return (unsigned short)(c * 65535 + 0.5);
}

/* ========== STATE ========== */
struct MiniEntry {
	Display *dpy;
	int screen;
	Window parent, win;
	ContextMenu *menu;
	GC gc;

	// Back buffers (Pixmaps)
	Pixmap back_pixmap; // entry buffer
	
	// DBE support
	DbeContext *dbe_ctx;
	XdbeBackBuffer dbe_back_buffer;
	int use_dbe; // 1 if DBE is available and initialized

	// Xft
	XftDraw *draw;
	XftFont *font;
	XftColor xft_fg;
	XftColor xft_selection_text; // text color when selected

	// Cached pixels
	unsigned long px_bg;
	unsigned long px_border;
	unsigned long original_border_px;
	unsigned long red_border_px;
	unsigned long green_border_px;
	unsigned long focus_border_px;
	unsigned long cursor_color_px; // cursor caret color
	unsigned long selection_color_px; // cached selection highlight pixel

	// Theming
	MiniTheme theme;
	MiniEntryConfig cfg;
	const CssBlock *entry_blk;
	
	// Geometry properties (passed at creation)
	int padding;
	int border_width;
	int border_radius;

	// Text buffer
	char *text;
	int text_len;
	int text_cap;
	int cursor;

	// Selection
	int sel_anchor, sel_active, selecting;

	// Clicks
	Time last_click_time;
	int last_click_x;
	int click_count;

	// Undo/Redo
	char **undo_stack;
	int undo_capacity;
	int undo_top;
	char **redo_stack;
	int redo_capacity;
	int redo_top;

	// Focus/blink
	int is_focused;
	int is_cursor_visible;
	int window_has_focus; // main window focus state
	long long last_blink_ms; // milliseconds since epoch

	// Geometry
	int x, y, w, h;

	// Scrolling
	int scroll_x;

	// Clipboard
	ClipboardContext *clipboard_ctx;
	Atom XA_CLIPBOARD; // Still needed for Ctrl+V detection

	// DAMAGE (union rect) for partial copy
	int dmg_x, dmg_y, dmg_w, dmg_h;

	// Validation and callbacks
	MiniEntryCallback on_change;
	void *user_data;
	int validation_state; // 0=normal, 1=invalid(red), 2=valid_flash(green)
	long long validation_flash_start;
};

static struct MiniEntry *focused_entry = NULL;

/* ========== TIME HELPERS ========== */
static long long get_time_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

/* ========== COLOR HELPERS ========== */
typedef struct {
	double r, g, b, a;
} RGBA;
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
	XColor xc;
	Colormap cmap = DefaultColormap(dpy, screen);
	xc.red = comp(c.r);
	xc.green = comp(c.g);
	xc.blue = comp(c.b);
	if (!XAllocColor(dpy, cmap, &xc)) {
		return BlackPixel(dpy, screen);
	}
	return xc.pixel;
}

/* ========== DAMAGE HELPERS ========== */
static void damage_reset(struct MiniEntry *e) {
	e->dmg_x = e->dmg_y = 0;
	e->dmg_w = e->dmg_h = 0;
}

static void damage_add(struct MiniEntry *e, int x, int y, int w, int h) {
	if (w <= 0 || h <= 0) {
		return;
	}
	if (e->dmg_w == 0 || e->dmg_h == 0) {
		e->dmg_x = x;
		e->dmg_y = y;
		e->dmg_w = w;
		e->dmg_h = h;
		return;
	}
	int x1 = (x < e->dmg_x) ? x : e->dmg_x;
	int y1 = (y < e->dmg_y) ? y : e->dmg_y;
	int x2 = ((x + w) > (e->dmg_x + e->dmg_w)) ? (x + w) : (e->dmg_x + e->dmg_w);
	int y2 = ((y + h) > (e->dmg_y + e->dmg_h)) ? (y + h) : (e->dmg_y + e->dmg_h);
	e->dmg_x = x1;
	e->dmg_y = y1;
	e->dmg_w = x2 - x1;
	e->dmg_h = y2 - y1;
}

static void damage_all(struct MiniEntry *e) {
	damage_add(e, 0, 0, e->w, e->h);
}

/* ========== FONTS / BUFFERS ========== */
static XftFont *open_font(Display *dpy, int screen, const char *fam, int size) {
	FcPattern *pat = FcNameParse((const FcChar8 *)(fam ? fam : "sans"));
	if (!pat) {
		return XftFontOpenName(dpy, screen, "sans-14");
	}
	FcPatternAddInteger(pat, FC_PIXEL_SIZE, size);
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

static Pixmap create_back_pixmap(Display *dpy, Window parent, int w, int h, int screen) {
	if (w <= 0) {
		w = 1;
	}
	if (h <= 0) {
		h = 1;
	}
	return XCreatePixmap(dpy, parent, (unsigned int)w, (unsigned int)h, (unsigned int)DefaultDepth(dpy, screen));
}

static void recreate_entry_buffers(struct MiniEntry *e) {
	// Clean up existing buffers
	if (e->back_pixmap) {
		XFreePixmap(e->dpy, e->back_pixmap);
	}
	if (e->dbe_back_buffer != None) {
		dbe_deallocate_back_buffer(e->dbe_ctx, e->dbe_back_buffer);
		e->dbe_back_buffer = None;
	}
	
	// Initialize DBE if available
	if (e->dbe_ctx && dbe_is_supported(e->dbe_ctx)) {
		e->dbe_back_buffer = dbe_allocate_back_buffer(e->dbe_ctx, e->win, XdbeUndefined);
		e->use_dbe = (e->dbe_back_buffer != None);
	} else {
		e->use_dbe = 0;
	}
	
	// Create pixmap fallback if DBE is not available
	if (!e->use_dbe) {
		e->back_pixmap = create_back_pixmap(e->dpy, e->win, e->w, e->h, e->screen);
	} else {
		e->back_pixmap = None;
	}
	
	// Recreate XftDraw context
	if (e->draw) {
		XftDrawDestroy(e->draw);
	}
	
	Drawable draw_target = e->use_dbe ? e->dbe_back_buffer : e->back_pixmap;
	e->draw = XftDrawCreate(e->dpy, draw_target, DefaultVisual(e->dpy, e->screen), DefaultColormap(e->dpy, e->screen));
}

/* ========== COLORS ========== */
static void cache_colors(struct MiniEntry *e) {
	RGBA rgba_tmp;

	memcpy(&rgba_tmp, &e->entry_blk->fg, sizeof(RGBA));
	e->xft_fg = xft_from_rgba(e->dpy, e->screen, rgba_tmp);

	memcpy(&rgba_tmp, &e->theme.selection_text_color, sizeof(RGBA));
	e->xft_selection_text = xft_from_rgba(e->dpy, e->screen, rgba_tmp);

	memcpy(&rgba_tmp, &e->entry_blk->bg, sizeof(RGBA));
	e->px_bg = to_px(e->dpy, e->screen, rgba_tmp);

	memcpy(&rgba_tmp, &e->entry_blk->border, sizeof(RGBA));
	e->px_border = to_px(e->dpy, e->screen, rgba_tmp);
	e->original_border_px = e->px_border;

	// Cursor color from theme
	memcpy(&rgba_tmp, &e->theme.cursor_color, sizeof(RGBA));
	e->cursor_color_px = to_px(e->dpy, e->screen, rgba_tmp);

	// Invalid border color from theme
	memcpy(&rgba_tmp, &e->entry_blk->invalid_border, sizeof(RGBA));
	e->red_border_px = to_px(e->dpy, e->screen, rgba_tmp);

	// Valid border color from theme
	memcpy(&rgba_tmp, &e->entry_blk->valid_border, sizeof(RGBA));
	e->green_border_px = to_px(e->dpy, e->screen, rgba_tmp);

	// Focus border color from theme
	memcpy(&rgba_tmp, &e->entry_blk->focus_border, sizeof(RGBA));
	e->focus_border_px = to_px(e->dpy, e->screen, rgba_tmp);

	// Selection color from theme
	memcpy(&rgba_tmp, &e->theme.selection_color, sizeof(RGBA));
	e->selection_color_px = to_px(e->dpy, e->screen, rgba_tmp);
}

/* ========== TEXT HELPERS ========== */
static void ensure_text(struct MiniEntry *e) {
	if (!e->text) {
		e->text = safe_strdup("");
		e->text_len = 0;
		e->text_cap = 1;
	}
}

static void ensure_text_len(struct MiniEntry *e) {
	ensure_text(e);
	int real = (int)strlen(e->text);
	if (real != e->text_len) {
		e->text_len = real;
	}
}

static void reserve_text(struct MiniEntry *e, int need) {
	if (need <= e->text_cap) {
		return;
	}
	int cap = e->text_cap > 0 ? e->text_cap : 1;
	while (cap < need) {
		cap = cap + cap / 2;
		if (cap < 8) {
			cap = 8;
		}
		if (cap > (1 << 30)) {
			break;
		}
	}
	e->text = safe_realloc(e->text, (size_t)cap);
	e->text_cap = cap;
}

/* ========== UNDO/REDO ========== */
static void undo_push(struct MiniEntry *e) {
	ensure_text(e);
	if (e->undo_top >= e->undo_capacity) {
		free(e->undo_stack[0]);
		memmove(e->undo_stack, e->undo_stack + 1, sizeof(char *) * (size_t)(e->undo_capacity - 1));
		e->undo_top = e->undo_capacity - 1;
	}
	e->undo_stack[e->undo_top++] = safe_strdup(e->text);
	for (int i = 0; i < e->redo_top; i++) {
		free(e->redo_stack[i]);
	}
	e->redo_top = 0;
}

static void do_undo(struct MiniEntry *e) {
	ensure_text(e);
	if (!e->undo_top) {
		return;
	}
	char *cur = safe_strdup(e->text);
	e->redo_stack[e->redo_top++] = cur;
	free(e->text);
	e->text = e->undo_stack[--e->undo_top];
	e->text_len = (int)strlen(e->text);
	e->text_cap = e->text_len + 1;
	if (e->cursor > e->text_len) {
		e->cursor = e->text_len;
	}
	e->sel_anchor = e->sel_active = e->cursor;
	if (e->on_change) {
		e->on_change(e, e->user_data);
	}
}

static void do_redo(struct MiniEntry *e) {
	ensure_text(e);
	if (!e->redo_top) {
		return;
	}
	char *cur = safe_strdup(e->text);
	e->undo_stack[e->undo_top++] = cur;
	free(e->text);
	e->text = e->redo_stack[--e->redo_top];
	e->text_len = (int)strlen(e->text);
	e->text_cap = e->text_len + 1;
	if (e->cursor > e->text_len) {
		e->cursor = e->text_len;
	}
	e->sel_anchor = e->sel_active = e->cursor;
	if (e->on_change) {
		e->on_change(e, e->user_data);
	}
}

/* ========== LAYOUT ========== */
static void update_fonts(struct MiniEntry *e) {
	if (e->font) {
		XftFontClose(e->dpy, e->font);
	}
	e->font = open_font(e->dpy, e->screen, e->entry_blk->font_family, e->entry_blk->font_size);

	int pad = e->padding;
	int new_h = e->font->ascent + e->font->descent + pad * 2 + 2;
	if (new_h < 22) {
		new_h = 22;
	}
	if (new_h != e->h) {
		e->h = new_h;
		XResizeWindow(e->dpy, e->win, (unsigned)e->w, (unsigned)e->h);
	}
}

/* ========== PAINT ========== */
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

static void draw_entry_bg(struct MiniEntry *e) {
	// Fill background only - border will be drawn directly to window
	XSetForeground(e->dpy, e->gc, e->px_bg);
	
	// Draw to appropriate buffer
	Drawable draw_target = e->use_dbe ? e->dbe_back_buffer : e->back_pixmap;
	XFillRectangle(e->dpy, draw_target, e->gc, 0, 0, (unsigned int)e->w, (unsigned int)e->h);
	damage_all(e);
}

static void draw_entry_border(struct MiniEntry *e) {
	unsigned long border_color = e->px_border;
	// Validation states take priority over focus
	if (e->validation_state == 1) {
		border_color = e->red_border_px;
	}
	else if (e->validation_state == 2) {
		border_color = e->green_border_px;
	}
	// Show focus border when focused (and not in validation state)
	else if (e->is_focused) {
		border_color = e->focus_border_px;
	}
	int radius = e->border_radius;
	int bw = e->border_width;
	if (bw < 0) {
		bw = 0;
	}
	// Draw rounded border directly to window (like button does)
	XSetForeground(e->dpy, e->gc, border_color);
	XSetLineAttributes(e->dpy, e->gc, (unsigned int)bw, LineSolid, CapButt, JoinMiter);
	int inset = bw / 2;
	draw_rounded_rect(e->dpy, e->win, e->gc, inset, inset, e->w - bw, e->h - bw, radius);
}

static void draw_selection(struct MiniEntry *e) {
	ensure_text_len(e);
	int a = e->sel_anchor, b = e->sel_active;
	if (a == b) {
		return;
	}
	if (a > b) {
		int t = a;
		a = b;
		b = t;
	}
	int pad = e->padding;
	XGlyphInfo A, B;
	XftTextExtentsUtf8(e->dpy, e->font, (const FcChar8 *)e->text, a, &A);
	XftTextExtentsUtf8(e->dpy, e->font, (const FcChar8 *)e->text, b, &B);
	int x0 = pad + 2 + A.xOff - e->scroll_x;
	int x1 = pad + 2 + B.xOff - e->scroll_x;

	// --- Centered baseline calculation (visual vertical alignment fix) ---
	int text_h = e->font->ascent + e->font->descent;
	int extra = e->h - (text_h + pad * 2);
	if (extra < 0) {
		extra = 0;
	}
	int baseline = pad + (extra / 2) + e->font->ascent;
	// ---------------------------------------------------------------------
	int h = e->font->ascent + e->font->descent;
	XSetForeground(e->dpy, e->gc, e->selection_color_px);
	int sx = x0;
	int sy = baseline - e->font->ascent;
	int sw = x1 - x0;
	int sh = h;
	
	// Draw to appropriate buffer
	Drawable draw_target = e->use_dbe ? e->dbe_back_buffer : e->back_pixmap;
	XFillRectangle(e->dpy, draw_target, e->gc, sx, sy, (unsigned)sw, (unsigned)sh);
	damage_add(e, sx, sy, sw, sh);
}

static void draw_text_and_cursor(struct MiniEntry *e) {
	ensure_text_len(e);
	int pad = e->padding;
	if (!e->draw) {
		return;
	}
	// --- Centered baseline calculation (visual vertical alignment fix) ---
	int text_h = e->font->ascent + e->font->descent;
	int extra = e->h - (text_h + pad * 2);
	if (extra < 0) {
		extra = 0;
	}
	int baseline = pad + (extra / 2) + e->font->ascent;
	// ---------------------------------------------------------------------

	int x_offset = pad + 2 - e->scroll_x;
	// Draw text with different colors for selected vs unselected portions
	if (e->sel_anchor != e->sel_active) {
		int a = e->sel_anchor, b = e->sel_active;
		if (a > b) {
			int t = a;
			a = b;
			b = t;
		}
		// Before selection
		if (a > 0) {
			XftDrawStringUtf8(e->draw, &e->xft_fg, e->font, x_offset, baseline, (const FcChar8 *)e->text, a);
		}
		// Selected text with different color
		if (b > a) {
			XGlyphInfo ext_before;
			XftTextExtentsUtf8(e->dpy, e->font, (const FcChar8 *)e->text, a, &ext_before);
			int sel_x = x_offset + ext_before.xOff;

			XftDrawStringUtf8(e->draw, &e->xft_selection_text, e->font, sel_x, baseline, (const FcChar8 *)(e->text + a), b - a);
		}
		// After selection
		if (b < e->text_len) {
			XGlyphInfo ext_before_end;
			XftTextExtentsUtf8(e->dpy, e->font, (const FcChar8 *)e->text, b, &ext_before_end);
			int after_x = x_offset + ext_before_end.xOff;

			XftDrawStringUtf8(e->draw, &e->xft_fg, e->font, after_x, baseline, (const FcChar8 *)(e->text + b), e->text_len - b);
		}
	}
	else {
		// No selection - draw all text normally
		XftDrawStringUtf8(e->draw, &e->xft_fg, e->font, x_offset, baseline, (const FcChar8 *)e->text, e->text_len);
	}
	damage_add(e, 1, 1, e->w - 2, e->h - 2);
	// Caret - only show if focused AND window has focus
	if (e->is_focused && e->window_has_focus) {
		int n = e->cursor;
		XGlyphInfo ext;
		XftTextExtentsUtf8(e->dpy, e->font, (const FcChar8 *)e->text, n, &ext);
		int cx = pad + 2 + ext.xOff - e->scroll_x;
		int cy0 = baseline - e->font->ascent;
		int cy1 = baseline + e->font->descent;
		int thickness = e->theme.cursor_thickness;
		if (e->is_cursor_visible) {
			XSetForeground(e->dpy, e->gc, e->cursor_color_px);
			for (int i = 0; i < thickness; i++) {
				Drawable draw_target = e->use_dbe ? e->dbe_back_buffer : e->back_pixmap;
				XDrawLine(e->dpy, draw_target, e->gc, cx + i, cy0, cx + i, cy1);
			}
		}
		damage_add(e, cx - 1, cy0, thickness + 2, cy1 - cy0 + 1);
	}
}

static void blit_damage(struct MiniEntry *e) {
	if (e->dmg_w <= 0 || e->dmg_h <= 0) {
		return;
	}
	if (!e->draw) {
		return;
	}
	
	// Use DBE swap if available, otherwise fallback to pixmap copy
	if (e->use_dbe) {
		// DBE: Swap buffers atomically
		dbe_swap_buffers(e->dbe_ctx, e->win, XdbeUndefined);
	} else {
		// Fallback: Copy from pixmap to window
		XSync(e->dpy, False);
		XCopyArea(e->dpy, e->back_pixmap, e->win, e->gc, e->dmg_x, e->dmg_y, (unsigned)e->dmg_w, (unsigned)e->dmg_h, e->dmg_x, e->dmg_y);
	}

	// Draw border directly to window after buffer swap/copy
	draw_entry_border(e);

	XFlush(e->dpy);
	damage_reset(e);
}

static void blit_damage_noflush(struct MiniEntry *e) {
	if (e->dmg_w <= 0 || e->dmg_h <= 0) {
		return;
	}
	if (!e->draw) {
		return;
	}
	
	// Use DBE swap if available, otherwise fallback to pixmap copy
	if (e->use_dbe) {
		// DBE: Swap buffers atomically (no flush needed)
		dbe_swap_buffers(e->dbe_ctx, e->win, XdbeUndefined);
	} else {
		// Fallback: Copy from pixmap to window
		XSync(e->dpy, False);
		XCopyArea(e->dpy, e->back_pixmap, e->win, e->gc, e->dmg_x, e->dmg_y, (unsigned)e->dmg_w, (unsigned)e->dmg_h, e->dmg_x, e->dmg_y);
	}

	// Draw border directly to window after buffer swap/copy
	draw_entry_border(e);

	// XFlush skipped - caller will flush once for all widgets to present atomically
	damage_reset(e);
}

static void redraw(struct MiniEntry *e) {
	draw_entry_bg(e);
	draw_selection(e);
	draw_text_and_cursor(e);
	blit_damage(e); // present entry
}

static void redraw_noflush(struct MiniEntry *e) {
	draw_entry_bg(e);
	draw_selection(e);
	draw_text_and_cursor(e);
	blit_damage_noflush(e); // present entry without flush
}

/* ========== SELECTION + EDITING ========== */
static void normalize_sel(struct MiniEntry *e) {
	ensure_text_len(e);
	if (e->sel_anchor < 0) {
		e->sel_anchor = 0;
	}
	if (e->sel_active < 0) {
		e->sel_active = 0;
	}
	if (e->sel_anchor > e->text_len) {
		e->sel_anchor = e->text_len;
	}
	if (e->sel_active > e->text_len) {
		e->sel_active = e->text_len;
	}
}

static void delete_selection(struct MiniEntry *e) {
	ensure_text_len(e);
	int a = e->sel_anchor, b = e->sel_active;
	if (a == b) {
		return;
	}
	if (a > b) {
		int t = a;
		a = b;
		b = t;
	}
	memmove(e->text + a, e->text + b, (size_t)(e->text_len - b + 1));
	e->text_len -= (b - a);
	e->cursor = a;
	e->sel_anchor = e->sel_active = a;
}

/* ========== VALIDATION ========== */
static int validate_char(struct MiniEntry *e, char ch, char *out) {
	switch (e->cfg.kind) {
		case ENTRY_TEXT:
			if (isprint((unsigned char)ch) && ch != '\r' && ch != '\n') {
				*out = ch;
				return 1;
			}
			return 0;
		case ENTRY_INT:
			if (isdigit((unsigned char)ch) || ch == ' ' || ch == ',') {
				*out = ch;
				return 1;
			}
			return 0;
		case ENTRY_FLOAT:
			if (ch == '.' || ch == ',' || ch == ' ') {
				*out = ch;
				return 1;
			}
			if (isdigit((unsigned char)ch)) {
				*out = ch;
				return 1;
			}
			return 0;
		case ENTRY_HEX:
			if (ch == '#') {
				*out = '#';
				return 1;
			}
			if (isxdigit((unsigned char)ch)) {
				*out = (char)(e->theme.hex_uppercase ? toupper((unsigned char)ch) : tolower((unsigned char)ch));
				return 1;
			}
			return 0;
	}
	return 0;
}

/* ========== MINIMAL VALIDATION ========== */

static void ensure_cursor_visible(struct MiniEntry *e) {
	int pad = e->padding;
	XGlyphInfo ext;
	XftTextExtentsUtf8(e->dpy, e->font, (const FcChar8 *)e->text, e->cursor, &ext);
	int cursor_x = pad + 2 + ext.xOff;

	int visible_w = e->w - pad * 2;
	int right_edge = e->scroll_x + visible_w - 8;
	int left_edge = e->scroll_x + pad;
	if (cursor_x > right_edge) {
		e->scroll_x = cursor_x - visible_w + 8;
	}
	else if (cursor_x < left_edge) {
		e->scroll_x = cursor_x - pad;
	}
	if (e->scroll_x < 0) {
		e->scroll_x = 0;
	}
	// clamp to max (no overscroll to blank)
	XGlyphInfo te;
	XftTextExtentsUtf8(e->dpy, e->font, (const FcChar8 *)e->text, e->text_len, &te);
	int text_w = te.xOff;
	int max_scroll = text_w - visible_w;
	if (max_scroll < 0) {
		max_scroll = 0;
	}
	if (e->scroll_x > max_scroll) {
		e->scroll_x = max_scroll;
	}
}

/* ========== WORD SELECTION (NON-MINIMAL) ========== */

/* Forward declaration for clipboard integration */
static void update_selection_clipboard(struct MiniEntry *e);

static int is_word_char(char c) {
	return isalnum((unsigned char)c) || c == '_';
}

static void select_word(struct MiniEntry *e, int pos) {
	ensure_text_len(e);
	if (pos < 0) {
		pos = 0;
	}
	if (pos > e->text_len) {
		pos = e->text_len;
	}
	int s = pos, t = pos;
	while (s > 0 && is_word_char(e->text[s - 1])) {
		s--;
	}
	while (t < e->text_len && is_word_char(e->text[t])) {
		t++;
	}
	e->sel_anchor = s;
	e->sel_active = t;
	e->cursor = t;
	ensure_cursor_visible(e);
	update_selection_clipboard(e);
}

static void select_all_text(struct MiniEntry *e) {
	ensure_text_len(e);
	e->sel_anchor = 0;
	e->sel_active = e->text_len;
	e->cursor = e->text_len;
	ensure_cursor_visible(e);
	update_selection_clipboard(e);
}

/* ========== CLIPBOARD SYSTEM (USING clipboard.c API) ========== */

/**
 * update_selection_clipboard - Auto-copy selection to PRIMARY if enabled
 *
 * Called when selection changes (mouse drag, keyboard selection, etc.)
 * Automatically copies selected text to PRIMARY selection for middle-click paste.
 */
static void update_selection_clipboard(struct MiniEntry *e) {
	if (!e->theme.auto_copy_primary) {
		return; // Feature disabled
	}
	if (e->sel_anchor == e->sel_active) {
		// No selection - clear PRIMARY
		clipboard_set_text(e->clipboard_ctx, e->win, NULL, SELECTION_PRIMARY);
	}
	else {
		// Selection exists - auto-copy to PRIMARY
		int a = e->sel_anchor, b = e->sel_active;
		if (a > b) {
			int t = a;
			a = b;
			b = t;
		}
		char *text = safe_strndup(e->text + a, (size_t)(b - a));
		clipboard_set_text(e->clipboard_ctx, e->win, text, SELECTION_PRIMARY);
		free(text);
	}
}

/**
 * copy_selection - Copy selected text to clipboard
 * @cut If true, delete selection after copying (cut operation)
 */
static void copy_selection(struct MiniEntry *e, int cut) {
	if (e->sel_anchor == e->sel_active) {
		return; // Nothing selected
	}
	int a = e->sel_anchor, b = e->sel_active;
	if (a > b) {
		int t = a;
		a = b;
		b = t;
	}
	char *text = safe_strndup(e->text + a, (size_t)(b - a));

	// Copy to both CLIPBOARD and PRIMARY
	clipboard_set_text(e->clipboard_ctx, e->win, text, SELECTION_CLIPBOARD);
	clipboard_set_text(e->clipboard_ctx, e->win, text, SELECTION_PRIMARY);

	free(text);
	if (cut) {
		undo_push(e);
		delete_selection(e);
		entry_draw(e);
	}
}

static void filtered_paste(struct MiniEntry *e, const unsigned char *data) {
	if (!data) {
		return;
	}
	size_t L = strlen((const char *)data);
	if (!L) {
		return;
	}
	char *out = (char *)malloc(L + 1);
	if (!out) {
		return;
	}
	int o = 0;
	// Compute effective base length if selection will be replaced
	int base_len = e->text_len;
	if (e->sel_anchor != e->sel_active) {
		int a = e->sel_anchor, b = e->sel_active;
		if (a > b) {
			int t = a;
			a = b;
			b = t;
		}
		base_len -= (b - a);
		if (base_len < 0) {
			base_len = 0;
		}
	}
	for (size_t i = 0; i < L; i++) {
		char ch = (char)data[i], v;
		if (validate_char(e, ch, &v)) {
			out[o++] = v;
			if (e->cfg.max_length > 0 && (base_len + o) >= e->cfg.max_length) {
				break;
			}
		}
	}
	if (o == 0) {
		free(out);
		return;
	}
	undo_push(e);
	if (e->sel_anchor != e->sel_active) {
		delete_selection(e);
	}
	if (e->text_len + o + 1 > e->text_cap) {
		e->text = (char *)safe_realloc(e->text, (size_t)(e->text_len + o + 1));
		e->text_cap = e->text_len + o + 1;
	}
	memmove(e->text + e->cursor + o, e->text + e->cursor, (size_t)(e->text_len - e->cursor + 1));
	memcpy(e->text + e->cursor, out, (size_t)o);
	e->text_len += o;
	e->cursor += o;
	free(out);
	ensure_cursor_visible(e);
	entry_draw(e);
}

/**
 * paste_callback - Clipboard paste callback
 *
 * Called by clipboard module when paste data arrives.
 */
static void paste_callback(const char *text, void *user_data) {
	struct MiniEntry *e = (struct MiniEntry *)user_data;
	if (text) {
		filtered_paste(e, (const unsigned char *)text);
	}
}

/**
 * paste_request - Request paste from clipboard or PRIMARY
 * @which XA_CLIPBOARD or XA_PRIMARY atom
 */
static void paste_request(struct MiniEntry *e, Atom which) {
	SelectionType type = (which == e->XA_CLIPBOARD) ?
	                     SELECTION_CLIPBOARD : SELECTION_PRIMARY;
	clipboard_request_text(e->clipboard_ctx, e->win, paste_callback, e, type);
}

/* ========== MAPPING x->index ========== */
static int x_to_index(struct MiniEntry *e, int x) {
	ensure_text_len(e);
	int pad = e->padding;
	for (int i = 0; i <= e->text_len; i++) {
		XGlyphInfo ext;
		XftTextExtentsUtf8(e->dpy, e->font, (const FcChar8 *)e->text, i, &ext);
		int pos = pad + 2 + ext.xOff;
		if (x < pos) {
			return i;
		}
	}
	return e->text_len;
}

/* ========== PUBLIC API ========== */

/**
 * @brief Create a new entry widget
 *
 * See entry.h for full documentation.
 * Initializes entry window, context menu, undo/redo buffers, and event handling.
 */
MiniEntry *entry_create(Display *dpy, int screen, Window parent, const MiniTheme *theme, const MiniEntryConfig *cfg, ClipboardContext *clipboard_ctx) {
	struct MiniEntry *e = (struct MiniEntry *)calloc(1, sizeof(*e));
	if (!e) {
		return NULL;
	}
	e->dpy = dpy;
	e->screen = screen;
	e->parent = parent;
	e->theme = *theme;
	e->cfg = *cfg;
	e->clipboard_ctx = clipboard_ctx;
	e->scroll_x = 0;
	e->window_has_focus = 1; // Assume window has focus initially
	switch (cfg->kind) {
		case ENTRY_TEXT:
			e->entry_blk = &e->theme.entry_text;
		break;
		case ENTRY_INT:
			e->entry_blk = &e->theme.entry_int;
		break;
		case ENTRY_FLOAT:
			e->entry_blk = &e->theme.entry_float;
		break;
		case ENTRY_HEX:
			e->entry_blk = &e->theme.entry_hex;
		break;
	}
	e->x = cfg->x_pos;
	e->y = cfg->y_pos;
	e->w = cfg->width;
	e->h = 22; // Initial height, will be auto-calculated by update_fonts()
	e->padding = cfg->padding;
	e->border_width = cfg->border_width;
	e->border_radius = cfg->border_radius;
	e->text = safe_strdup("");
	e->text_len = 0;
	e->text_cap = 1;
	e->cursor = 0;
	e->sel_anchor = e->sel_active = 0;
	e->undo_capacity = e->theme.undo_depth;
	e->redo_capacity = e->theme.undo_depth;
	e->undo_stack = (char **)calloc((size_t)e->undo_capacity, sizeof(char *));
	e->redo_stack = (char **)calloc((size_t)e->redo_capacity, sizeof(char *));
	if (!e->undo_stack || !e->redo_stack) {
		free(e->undo_stack);
		free(e->redo_stack);
		free(e->text);
		free(e);
		return NULL;
	}
	e->undo_top = e->redo_top = 0;
	e->XA_CLIPBOARD = XInternAtom(dpy, "CLIPBOARD", False);

	e->on_change = cfg->on_change;
	e->user_data = cfg->user_data;
	e->validation_state = 0;
	e->validation_flash_start = 0;

	// Initialize DBE context
	e->dbe_ctx = dbe_init(dpy, screen);
	e->dbe_back_buffer = None;
	e->use_dbe = 0;

	// Windows
	XSetWindowAttributes a;
	a.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
	               KeyPressMask | FocusChangeMask | StructureNotifyMask | PropertyChangeMask |
	               SubstructureNotifyMask;
	a.background_pixmap = None;
	
	e->win = XCreateWindow(dpy, parent, e->x, e->y, (unsigned)e->w, (unsigned)e->h, 0,
	                       CopyFromParent, InputOutput, CopyFromParent,
	                       CWEventMask | CWBackPixmap, &a);

	e->gc = XCreateGC(dpy, e->win, 0, NULL);

	// Set I-beam cursor for text entry
	Cursor text_cursor = XCreateFontCursor(dpy, XC_xterm);
	XDefineCursor(dpy, e->win, text_cursor);
	XFreeCursor(dpy, text_cursor);

	// Menu
	e->menu = menu_create(dpy, screen, &e->theme);

	// Fonts
	update_fonts(e);

	// Buffers
	recreate_entry_buffers(e);

	cache_colors(e);

	damage_reset(e);

	XMapWindow(dpy, e->win);

	// Force an initial paint so widgets appear immediately
	entry_draw(e);
	XFlush(dpy);

	return e;
}

/**
 * @brief Destroy entry widget and free resources
 *
 * See entry.h for full documentation.
 */
void entry_destroy(struct MiniEntry *e) {
	if (!e) {
		return;
	}
	if (focused_entry == e) {
		focused_entry = NULL;
	}
	menu_destroy(e->menu);
	// Clean up DBE resources
	if (e->dbe_back_buffer != None) {
		dbe_deallocate_back_buffer(e->dbe_ctx, e->dbe_back_buffer);
	}
	if (e->dbe_ctx) {
		dbe_destroy(e->dbe_ctx);
	}
	// Free Xft colors
	XftColorFree(e->dpy, DefaultVisual(e->dpy, e->screen), DefaultColormap(e->dpy, e->screen), &e->xft_fg);
	XftColorFree(e->dpy, DefaultVisual(e->dpy, e->screen), DefaultColormap(e->dpy, e->screen), &e->xft_selection_text);
	if (e->draw) {
		XftDrawDestroy(e->draw);
	}
	if (e->back_pixmap) {
		XFreePixmap(e->dpy, e->back_pixmap);
	}
	if (e->font) {
		XftFontClose(e->dpy, e->font);
	}
	if (e->gc) {
		XFreeGC(e->dpy, e->gc);
	}
	if (e->win) {
		XDestroyWindow(e->dpy, e->win);
	}
	for (int i = 0; i < e->undo_top; i++) {
		free(e->undo_stack[i]);
	}
	for (int i = 0; i < e->redo_top; i++) {
		free(e->redo_stack[i]);
	}
	free(e->undo_stack);
	free(e->redo_stack);
	free(e->text);
	free(e);
}

/**
 * @brief Check if entry is focused
 *
 * See entry.h for full documentation.
 */
// cppcheck-suppress unusedFunction
int entry_is_focused_check(const struct MiniEntry *e) {
	return (focused_entry == e);
}

/**
 * @brief Set entry focus state
 *
 * See entry.h for full documentation.
 */
// cppcheck-suppress unusedFunction
void entry_focus(struct MiniEntry *e, int f) {
	if (f) {
		if (focused_entry && focused_entry != e) {
			focused_entry->is_focused = 0;
			focused_entry->is_cursor_visible = 0;
			entry_draw(focused_entry);
		}
		focused_entry = e;
		e->is_focused = 1;
		e->is_cursor_visible = 1;
		e->last_blink_ms = get_time_ms();

		XSetInputFocus(e->dpy, e->win, RevertToNone, CurrentTime);
	}
	else {
		if (focused_entry == e) {
			// Validate on focus loss
			if (e->on_change) {
				e->on_change(e, e->user_data);
			}
			focused_entry = NULL;
			e->is_focused = 0;
			e->is_cursor_visible = 0;
		}
	}
	entry_draw(e);
}

void entry_draw(struct MiniEntry *e) {
	damage_all(e);
	redraw(e);
	XFlush(e->dpy);
}

void entry_draw_noflush(struct MiniEntry *e) {
	damage_all(e);
	redraw_noflush(e);
	// XFlush skipped - caller will flush once for all widgets
}

// cppcheck-suppress unusedFunction
void entry_set_theme(struct MiniEntry *e, const MiniTheme *theme) {
	e->theme = *theme;
	switch (e->cfg.kind) {
		case ENTRY_TEXT:
			e->entry_blk = &e->theme.entry_text;
		break;
		case ENTRY_INT:
			e->entry_blk = &e->theme.entry_int;
		break;
		case ENTRY_FLOAT:
			e->entry_blk = &e->theme.entry_float;
		break;
		case ENTRY_HEX:
			e->entry_blk = &e->theme.entry_hex;
		break;
	}
	// Recreate context menu to apply new theme->menu font/size/colors
	if (e->menu) {
		menu_destroy(e->menu);
	}
	e->menu = menu_create(e->dpy, e->screen, &e->theme);
	update_fonts(e);
	recreate_entry_buffers(e);
	cache_colors(e);
	entry_draw(e);
}

void entry_set_theme_noflush(struct MiniEntry *e, const MiniTheme *theme) {
	e->theme = *theme;
	switch (e->cfg.kind) {
		case ENTRY_TEXT:
			e->entry_blk = &e->theme.entry_text;
		break;
		case ENTRY_INT:
			e->entry_blk = &e->theme.entry_int;
		break;
		case ENTRY_FLOAT:
			e->entry_blk = &e->theme.entry_float;
		break;
		case ENTRY_HEX:
			e->entry_blk = &e->theme.entry_hex;
		break;
	}
	if (e->menu) {
		menu_destroy(e->menu);
	}
	e->menu = menu_create(e->dpy, e->screen, &e->theme);
	update_fonts(e);
	recreate_entry_buffers(e);
	cache_colors(e);
	entry_draw_noflush(e);
}

// cppcheck-suppress unusedFunction
void entry_move(struct MiniEntry *e, int x, int y) {
	e->x = x;
	e->y = y;
	XMoveWindow(e->dpy, e->win, x, y);
	entry_draw(e);
}

void entry_move_noflush(struct MiniEntry *e, int x, int y) {
	e->x = x;
	e->y = y;
	XMoveWindow(e->dpy, e->win, x, y);
	entry_draw_noflush(e);
}

// cppcheck-suppress unusedFunction
void entry_resize(struct MiniEntry *e, int w, int h) {
	if (w <= 0) {
		w = e->w;
	}
	if (h <= 0) {
		h = e->h;
	}
	if (w == e->w && h == e->h) {
		return;
	}
	e->w = w;
	e->h = h;
	XResizeWindow(e->dpy, e->win, (unsigned)w, (unsigned)h);
	recreate_entry_buffers(e);
	entry_draw(e);
}

void entry_resize_noflush(struct MiniEntry *e, int w, int h) {
	if (w <= 0) {
		w = e->w;
	}
	if (h <= 0) {
		h = e->h;
	}
	if (w == e->w && h == e->h) {
		return;
	}
	e->w = w;
	e->h = h;
	XResizeWindow(e->dpy, e->win, (unsigned)w, (unsigned)h);
	recreate_entry_buffers(e);
	entry_draw_noflush(e);
}

void entry_set_text(struct MiniEntry *e, const char *t) {
	if (!t) {
		t = "";
	}
	free(e->text);
	e->text = safe_strdup(t);
	e->text_len = (int)strlen(e->text);
	e->text_cap = e->text_len + 1;
	e->cursor = e->text_len;
	e->sel_anchor = e->sel_active = e->cursor;
	entry_draw(e);
}

void entry_set_text_no_draw(struct MiniEntry *e, const char *t) {
	if (!t) {
		t = "";
	}
	free(e->text);
	e->text = safe_strdup(t);
	e->text_len = (int)strlen(e->text);
	e->text_cap = e->text_len + 1;
	e->cursor = e->text_len;
	e->sel_anchor = e->sel_active = e->cursor;
	// No draw - caller will handle
}

const char *entry_get_text(struct MiniEntry *e) {
	ensure_text_len(e);
	return e->text;
}

/* ========== KEY HANDLING ========== */

/* --- Text Editing Functions --- */

static void insert_char(struct MiniEntry *e, char ch) {
	ensure_text_len(e);
	char out;
	if (!validate_char(e, ch, &out)) {
		return;
	}
	undo_push(e);
	if (e->sel_anchor != e->sel_active) {
		delete_selection(e);
	}
	// Check max_len after deleting selection
	if (e->cfg.max_length > 0 && e->text_len >= e->cfg.max_length) {
		return;
	}
	reserve_text(e, e->text_len + 2);
	memmove(e->text + e->cursor + 1, e->text + e->cursor, (size_t)(e->text_len - e->cursor + 1));
	e->text[e->cursor] = out;
	e->text_len++;
	e->cursor++;
	ensure_cursor_visible(e);
	entry_draw(e);
}

static void do_backspace(struct MiniEntry *e) {
	ensure_text_len(e);
	if (e->sel_anchor != e->sel_active) {
		undo_push(e);
		delete_selection(e);
		entry_draw(e);
		return;
	}
	if (e->cursor > 0) {
		undo_push(e);
		memmove(e->text + e->cursor - 1, e->text + e->cursor, (size_t)(e->text_len - e->cursor + 1));
		e->text_len--;
		e->cursor--;
		ensure_cursor_visible(e);
		entry_draw(e);
	}
}

static void do_delete(struct MiniEntry *e) {
	ensure_text_len(e);
	if (e->sel_anchor != e->sel_active) {
		undo_push(e);
		delete_selection(e);
		entry_draw(e);
		return;
	}
	if (e->cursor < e->text_len) {
		undo_push(e);
		memmove(e->text + e->cursor, e->text + e->cursor + 1, (size_t)(e->text_len - e->cursor));
		e->text_len--;
		ensure_cursor_visible(e);
		entry_draw(e);
	}
}

/* --- Keyboard Shortcuts Handler --- */

static void key_shortcuts(struct MiniEntry *e, XKeyEvent *kev) {
	KeySym ks;
	char buf[8];
	int n = XLookupString(kev, buf, sizeof(buf), &ks, NULL);
	int ctrl = (kev->state & ControlMask) != 0;
	int shift = (kev->state & ShiftMask) != 0;
	if (ks == XK_Return || ks == XK_KP_Enter) {
		if (e->on_change) {
			e->on_change(e, e->user_data);
		}
		return;
	}
	
	/* Clipboard Shortcuts */
	// Ctrl+C - Copy
	if (ctrl && (ks == XK_c || ks == XK_C)) {
		copy_selection(e, 0);
		return;
	}
	// Ctrl+X - Cut
	if (ctrl && (ks == XK_x || ks == XK_X)) {
		copy_selection(e, 1);
		return;
	}
	// Ctrl+V - Paste
	if (ctrl && (ks == XK_v || ks == XK_V)) {
		paste_request(e, e->XA_CLIPBOARD);
		return;
	}
	
	/* Selection Shortcuts */
	// Ctrl+A - Select All
	if (ctrl && (ks == XK_a || ks == XK_A)) {
		ensure_text_len(e);
		e->sel_anchor = 0;
		e->sel_active = e->text_len;
		e->cursor = e->text_len;
		ensure_cursor_visible(e);
		entry_draw(e);
		return;
	}
	
	/* Undo/Redo Shortcuts */
	// Ctrl+Z - Undo
	if (ctrl && (ks == XK_z || ks == XK_Z)) {
		do_undo(e);
		ensure_cursor_visible(e);
		entry_draw(e);
		return;
	}
	// Ctrl+Y - Redo
	if (ctrl && (ks == XK_y || ks == XK_Y)) {
		do_redo(e);
		ensure_cursor_visible(e);
		entry_draw(e);
		return;
	}
	
	/* Navigation Keys */
	switch (ks) {
		case XK_Left:
			ensure_text_len(e);
			if (shift) {
				if (e->cursor > 0) {
					e->cursor--;
					e->sel_active = e->cursor;
				}
			}
			else {
				if (e->cursor > 0) {
					e->cursor--;
				}
				e->sel_anchor = e->cursor;
				e->sel_active = e->cursor;
			}
			ensure_cursor_visible(e);
			entry_draw(e);
		break;
		case XK_Right:
			ensure_text_len(e);
			if (shift) {
				if (e->cursor < e->text_len) {
					e->cursor++;
					e->sel_active = e->cursor;
				}
			}
			else {
				if (e->cursor < e->text_len) {
					e->cursor++;
				}
				e->sel_anchor = e->cursor;
				e->sel_active = e->cursor;
			}
			ensure_cursor_visible(e);
			entry_draw(e);
		break;
		case XK_Home:
			ensure_text_len(e);
			e->cursor = 0;
			if (!shift) {
				e->sel_anchor = e->sel_active = 0;
			}
			else {
				e->sel_active = 0;
			}
			ensure_cursor_visible(e);
			entry_draw(e);
		break;
		case XK_End:
			ensure_text_len(e);
			e->cursor = e->text_len;
			if (!shift) {
				e->sel_anchor = e->cursor;
				e->sel_active = e->cursor;
			}
			else {
				e->sel_active = e->cursor;
			}
			ensure_cursor_visible(e);
			entry_draw(e);
		break;
		case XK_BackSpace:
			do_backspace(e);
		break;
		case XK_Delete:
			do_delete(e);
		break;
		default:
			if (n == 1 && (unsigned char)buf[0] >= 32 && (unsigned char)buf[0] < 127) {
				insert_char(e, buf[0]);
			}
		break;
	}
}

/* ========== EVENTS ========== */

int entry_handle_event(struct MiniEntry *e, XEvent *ev) {
	/* --- Context Menu Events --- */
	if (menu_is_visible(e->menu)) {
		int has_selection = (e->sel_anchor != e->sel_active);
		int can_cut = has_selection;
		int can_copy = has_selection;
		int can_paste = (XGetSelectionOwner(e->dpy, e->XA_CLIPBOARD) != None);
		int can_select_all = (e->text_len > 0); // Can select all if there's text
		int can_clear = (e->text_len > 0);
		int can_undo = e->undo_top > 0;
		int can_redo = e->redo_top > 0;
		int act = menu_handle_event(e->menu, ev, can_cut, can_copy, can_paste, can_select_all, can_clear, can_undo, can_redo);
		if (act >= 0) {
			switch (act) {
				case 0: // Cut
					copy_selection(e, 1);
				break;
				case 1: // Copy
					copy_selection(e, 0);
				break;
				case 2: // Paste
					if (can_paste) {
						paste_request(e, e->XA_CLIPBOARD);
					}
				break;
				case 3: // Select All
					if (can_select_all) {
						select_all_text(e);
					}
				break;
				case 4: // Clear
					if (can_clear) {
						undo_push(e);
						free(e->text);
						e->text = safe_strdup("");
						e->text_len = 0;
						e->text_cap = 1;
						e->cursor = 0;
						e->sel_anchor = e->sel_active = 0;
						// Ensure entry remains focused after clear
						focused_entry = e;
						e->is_focused = 1;
						e->is_cursor_visible = 1;
						e->last_blink_ms = get_time_ms();
						ensure_cursor_visible(e);
						entry_draw(e);
						// Note: validation will happen on focus loss if text is still empty
					}
				break;
				case 5: // Undo
					if (can_undo) {
						do_undo(e);
					}
				break;
				case 6: // Redo
					if (can_redo) {
						do_redo(e);
					}
				break;
			}
			menu_hide(e->menu);
			entry_draw(e);
			return 1;
		}
		if (ev->type == ButtonPress && ev->xany.window != menu_get_window(e->menu)) {
			menu_hide(e->menu);
		}
		if (ev->xany.window == menu_get_window(e->menu)) {
			return 1;
		}
	}
	
	/* --- Expose Events --- */
	if (ev->type == Expose) {
		if (ev->xexpose.window == e->win) {
			if (ev->xexpose.count == 0) {
				damage_all(e);
				redraw(e);
			}
			return 1;
		}
	}
	
	/* --- Mouse Button Events --- */
	else if (ev->type == ButtonPress) {
		if (ev->xbutton.window == e->win) {
			if (ev->xbutton.button == Button1) {
				if (focused_entry && focused_entry != e) {
					// Properly unfocus previous entry (triggers validation)
					entry_focus(focused_entry, 0);
				}
				focused_entry = e;
				e->is_focused = 1;
				e->is_cursor_visible = 1;
				e->last_blink_ms = get_time_ms();

				int lx = ev->xbutton.x;
				int click_pos = x_to_index(e, lx + e->scroll_x);

				Time now = ev->xbutton.time;
				int dbl = 400;
				if (now - e->last_click_time < (Time)dbl && abs(ev->xbutton.x - e->last_click_x) < 5) {
					e->click_count++;
				}
				else {
					e->click_count = 1;
				}
				e->last_click_time = now;
				e->last_click_x = ev->xbutton.x;
				if (e->click_count == 1) {
					e->cursor = click_pos;
					e->sel_anchor = e->sel_active = e->cursor;
					e->selecting = 1;
				}
				else if (e->click_count == 2) {
					select_word(e, click_pos);
					e->selecting = 0;
				}
				else {
					select_all_text(e);
					e->selecting = 0;
					e->click_count = 0;
				}
				ensure_cursor_visible(e);
				menu_hide(e->menu);
				entry_draw(e);
			}
			else if (ev->xbutton.button == Button2) {
				// Middle-click paste from PRIMARY
				if (focused_entry && focused_entry != e) {
					// Properly unfocus previous entry (triggers validation)
					entry_focus(focused_entry, 0);
				}
				focused_entry = e;
				e->is_focused = 1;
				e->is_cursor_visible = 1;
				e->last_blink_ms = get_time_ms();

				int click_pos = x_to_index(e, ev->xbutton.x + e->scroll_x);
				e->cursor = click_pos;
				e->sel_anchor = e->sel_active = e->cursor;
				ensure_cursor_visible(e);
				menu_hide(e->menu);
				entry_draw(e);
				paste_request(e, XA_PRIMARY);
			}
			else if (ev->xbutton.button == Button3) {
				menu_show(e->menu, ev->xbutton.x_root, ev->xbutton.y_root);
			}
			return 1;
		}
	}
	else if (ev->type == ButtonRelease) {
		if (ev->xbutton.window == e->win && ev->xbutton.button == Button1) {
			e->selecting = 0;
			update_selection_clipboard(e);
		}
	}
	
	/* --- Mouse Motion Events --- */
	else if (ev->type == MotionNotify) {
		if (e->selecting && ev->xmotion.window == e->win) {
			e->sel_active = x_to_index(e, ev->xmotion.x + e->scroll_x);
			e->cursor = e->sel_active;
			normalize_sel(e);
			ensure_cursor_visible(e);
			update_selection_clipboard(e);
			// Auto-scroll when dragging out of bounds
			if (ev->xmotion.x < 0) {
				e->scroll_x -= 10;
			}
			else if (ev->xmotion.x > e->w) {
				e->scroll_x += 10;
			}
			if (e->scroll_x < 0) {
				e->scroll_x = 0;
			}
			// clamp right
			XGlyphInfo ext;
			XftTextExtentsUtf8(e->dpy, e->font, (const FcChar8 *)e->text, e->text_len, &ext);
			int text_w = ext.xOff;
			int visible_w = e->w - e->padding * 2;
			int max_scroll = text_w - visible_w;
			if (max_scroll < 0) {
				max_scroll = 0;
			}
			if (e->scroll_x > max_scroll) {
				e->scroll_x = max_scroll;
			}
			entry_draw(e);
		}
	}
	
	/* --- Keyboard Events --- */
	else if (ev->type == KeyPress && e->is_focused) {
		key_shortcuts(e, &ev->xkey);
		e->is_cursor_visible = 1;
		e->last_blink_ms = get_time_ms();
		return 1;
	}
	
	/* --- Focus Events --- */
	else if (ev->type == FocusOut) {
		if (ev->xfocus.window == e->win) {
			// Validate on focus loss
			if (e->on_change) {
				e->on_change(e, e->user_data);
			}
			e->is_focused = 0;
			e->is_cursor_visible = 0;
			menu_hide(e->menu);
			entry_draw(e);
		}
	}
	else if (ev->type == FocusIn) {
		if (ev->xfocus.window == e->win) {
			if (focused_entry && focused_entry != e) {
				// Properly unfocus previous entry (triggers validation)
				entry_focus(focused_entry, 0);
			}
			focused_entry = e;
			e->is_focused = 1;
			e->is_cursor_visible = 1;
			e->last_blink_ms = get_time_ms();

			entry_draw(e);
		}
	}
	else if (ev->type == ConfigureNotify) {
		if (ev->xconfigure.window == e->win) {
			e->w = ev->xconfigure.width;
			e->h = ev->xconfigure.height;
			recreate_entry_buffers(e);
			entry_draw(e);
		}
	}
	// SelectionRequest and SelectionNotify now handled by clipboard.c
	return 0;
}

void entry_update_blink(struct MiniEntry *e) {
	if (!e || !e->is_focused || !e->window_has_focus) {
		return;
	}
	long long now = get_time_ms();
	long long blink_interval = (long long)e->theme.cursor_blink_ms;
	if (now - e->last_blink_ms >= blink_interval) {
		e->is_cursor_visible = !e->is_cursor_visible;
		e->last_blink_ms = now;
		entry_draw(e);
	}
}

// cppcheck-suppress unusedFunction
void entry_set_callback(MiniEntry *e, MiniEntryCallback cb, void *user_data) {
	if (!e) {
		return;
	}
	e->on_change = cb;
	e->user_data = user_data;
}

/* ========== VALIDATION STATE ACCESSORS ========== */

void entry_set_validation_state(MiniEntry *e, int state) {
	if (!e) {
		return;
	}
	e->validation_state = state;
	entry_draw(e);
}

int entry_get_validation_state(const MiniEntry *e) {
	if (!e) {
		return 0;
	}
	return e->validation_state;
}

long long entry_get_validation_flash_start(const MiniEntry *e) {
	if (!e) {
		return 0;
	}
	return e->validation_flash_start;
}

void entry_set_validation_flash_start(MiniEntry *e, long long timestamp) {
	if (!e) {
		return;
	}
	e->validation_flash_start = timestamp;
}

/**
 * @brief Handle window focus changes
 *
 * See entry.h for full documentation.
 * Manages cursor visibility based on parent window focus state.
 */
void entry_handle_window_focus(MiniEntry *e, int has_focus) {
	if (!e) {
		return;
	}
	e->window_has_focus = has_focus;
	// If window loses focus, hide cursor
	if (!has_focus) {
		e->is_cursor_visible = 0;
	}
	else if (e->is_focused) {
		// Window regained focus and this entry is focused - show cursor
		e->is_cursor_visible = 1;
		e->last_blink_ms = get_time_ms();
	}
	entry_draw(e);
}

/* ========== CONFIGURATION MANAGEMENT ========== */

void entry_config_init_defaults(struct EntryBlock *block, const char *entry_type) {
	(void)entry_type;
	strncpy(block->font_family, "DejaVu Sans", sizeof(block->font_family) - 1);
	block->font_size = 16;
	block->fg = (ConfigColor){0.180f, 0.204f, 0.212f, 1.0f};  // #2E3436 dark slate
	block->bg = (ConfigColor){1.0f, 1.0f, 1.0f, 1.0f};  // #FFFFFF white
	block->border = (ConfigColor){0.804f, 0.780f, 0.761f, 1.0f};  // #CDC7C2 light gray
	block->valid_border = (ConfigColor){0.149f, 0.635f, 0.412f, 1.0f};  // #26A269 success green
	block->invalid_border = (ConfigColor){0.878f, 0.106f, 0.141f, 1.0f};  // #E01B24 error red
	block->focus_border = (ConfigColor){0.804f, 0.780f, 0.761f, 1.0f};  // #CDC7C2 (same as border)
}

// cppcheck-suppress unusedFunction
void entry_config_parse(struct EntryBlock *block, const char *key, const char *value) {
	if (strcmp(key, "font") == 0 || strcmp(key, "font-family") == 0) {
		strncpy(block->font_family, value, sizeof(block->font_family) - 1);
	}
	else if (strcmp(key, "invalid-border") == 0) {
		block->invalid_border = parse_color(value);
	}
	else if (strcmp(key, "focus-border") == 0) {
		block->focus_border = parse_color(value);
	}
	else if (strcmp(key, "color") == 0) {
		unsigned int r = 0, g = 0, b = 0;
		if (sscanf(value, "#%02x%02x%02x", &r, &g, &b) == 3) {
			block->fg.r = (float)r / 255.0f;
			block->fg.g = (float)g / 255.0f;
			block->fg.b = (float)b / 255.0f;
			block->fg.a = 1.0f;
		}
	}
	else if (strcmp(key, "background") == 0) {
		unsigned int r = 0, g = 0, b = 0;
		if (sscanf(value, "#%02x%02x%02x", &r, &g, &b) == 3) {
			block->bg.r = (float)r / 255.0f;
			block->bg.g = (float)g / 255.0f;
			block->bg.b = (float)b / 255.0f;
			block->bg.a = 1.0f;
		}
	}
	else if (strcmp(key, "border") == 0) {
		unsigned int r = 0, g = 0, b = 0;
		if (sscanf(value, "#%02x%02x%02x", &r, &g, &b) == 3) {
			block->border.r = (float)r / 255.0f;
			block->border.g = (float)g / 255.0f;
			block->border.b = (float)b / 255.0f;
			block->border.a = 1.0f;
		}
	}
	else if (strcmp(key, "valid-border") == 0 || strcmp(key, "hover-border") == 0) {
		unsigned int r = 0, g = 0, b = 0;
		if (sscanf(value, "#%02x%02x%02x", &r, &g, &b) == 3) {
			block->valid_border.r = (float)r / 255.0f;
			block->valid_border.g = (float)g / 255.0f;
			block->valid_border.b = (float)b / 255.0f;
			block->valid_border.a = 1.0f;
		}
	}
	else if (strcmp(key, "invalid-border") == 0 || strcmp(key, "active-border") == 0) {
		unsigned int r = 0, g = 0, b = 0;
		if (sscanf(value, "#%02x%02x%02x", &r, &g, &b) == 3) {
			block->invalid_border.r = (float)r / 255.0f;
			block->invalid_border.g = (float)g / 255.0f;
			block->invalid_border.b = (float)b / 255.0f;
			block->invalid_border.a = 1.0f;
		}
	}
}

void entry_config_write(FILE *f, const struct EntryBlock *block, const char *entry_type) {
	fprintf(f, "[%s]\n", entry_type);
	fprintf(f, "background = #%02X%02X%02X\n",
		(int)(block->bg.r * 255),
		(int)(block->bg.g * 255),
		(int)(block->bg.b * 255));
	fprintf(f, "border = #%02X%02X%02X\n",
		(int)(block->border.r * 255),
		(int)(block->border.g * 255),
		(int)(block->border.b * 255));
	fprintf(f, "color = #%02X%02X%02X\n",
		(int)(block->fg.r * 255),
		(int)(block->fg.g * 255),
		(int)(block->fg.b * 255));
	fprintf(f, "focus-border = #%02X%02X%02X\n",
		(int)(block->focus_border.r * 255),
		(int)(block->focus_border.g * 255),
		(int)(block->focus_border.b * 255));
	fprintf(f, "font = %s\n", block->font_family);
	fprintf(f, "font-size = %d\n", block->font_size);
	fprintf(f, "invalid-border = #%02X%02X%02X\n",
		(int)(block->invalid_border.r * 255),
		(int)(block->invalid_border.g * 255),
		(int)(block->invalid_border.b * 255));
	fprintf(f, "valid-border = #%02X%02X%02X\n\n",
		(int)(block->valid_border.r * 255),
		(int)(block->valid_border.g * 255),
		(int)(block->valid_border.b * 255));
}
