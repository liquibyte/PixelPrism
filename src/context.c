/* context.c - Popup Context Menu Implementation
 *
 * Implements a simple popup menu with hover highlighting, separators, and
 * item enable/disable states. Used for right-click context menus in entry widgets.
 *
 * Menu structure:
 * - Cut, Copy, Paste (edit operations)
 * - [separator]
 * - Clear (delete all text)
 * - [separator]
 * - Undo, Redo (history operations)
 *
 * Each item can be independently enabled/disabled based on application state.
 *
 * Internal design notes:
 * - The menu is an override-redirect Window drawn with Xft for text.
 * - Items are stored in a simple array; separators share the same struct with
 *   a flag. Hover state is tracked by index; clicks map directly to callbacks.
 * - Shadow/rounded corners use the SHAPE extension when available.
 */

#include "context.h"
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/shape.h>
#include <fontconfig/fontconfig.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========== CONSTANTS ========== */

#define MENU_WIDTH 160 // Fixed menu width in pixels
#define MENU_HEIGHT 140 // Base menu height
#define ITEM_HEIGHT 22 // Height of each menu item

/* Context menu internal structure */
struct ContextMenu {
	// X11 resources
	Display *dpy;
	int screen;
	Window win;
	GC gc;
	MenuBlock style;

	// State
	int is_visible; // 1 if menu is currently shown
	int hover_index; // Index of item under mouse (-1 if none)
	int is_active; // 1 during mouse press

	// Cached colors
	unsigned long fg, bg, border, hover_bg;

	// Font rendering
	XftDraw *draw;
	XftFont *font;
	XftColor xft_fg;
	XftColor xft_disabled;

	// Geometry
	int item_height;
	int menu_height;
	int border_width;
	int border_radius;
	int padding;

	// Menu items
	char menu_items[7][32]; // Labels for 7 menu items
	int menu_item_count; // Always 7 for this implementation
};

/* ========== HELPER FUNCTIONS ========== */

/* Color conversion and font loading now centralized in config.c */

/* Draw a rounded rectangle border
 * Falls back to regular rectangle if radius is 0 or too large for dimensions.
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

/* Fill a rounded rectangle for shape mask
 * Fills interior and draws exact outline matching draw_rounded_rect.
 */
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
	// Draw the EXACT same outline as draw_rounded_rect
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

/* Fill a rounded rectangle with selective corner rounding for hover effects
 * round_top: 1 to round top corners, 0 to square them
 * round_bottom: 1 to round bottom corners, 0 to square them
 */
static void fill_rounded_rect_selective(Display *dpy, Drawable d, GC gc, int x, int y, int w, int h, int radius, int round_top, int round_bottom) {
	if (radius <= 0 || (!round_top && !round_bottom)) {
		XFillRectangle(dpy, d, gc, x, y, (unsigned int)w, (unsigned int)h);
		return;
	}
	if (radius * 2 > w || radius * 2 > h) {
		XFillRectangle(dpy, d, gc, x, y, (unsigned int)w, (unsigned int)h);
		return;
	}
	
	int diameter = radius * 2;
	
	// Fill the center rectangle
	int fill_y = round_top ? (y + radius) : y;
	int fill_h = h - (round_top ? radius : 0) - (round_bottom ? radius : 0);
	if (fill_h > 0) {
		XFillRectangle(dpy, d, gc, x, fill_y, (unsigned int)w, (unsigned int)fill_h);
	}
	
	// Fill top rectangles if rounding top
	if (round_top) {
		XFillRectangle(dpy, d, gc, x + radius, y, (unsigned int)(w - diameter), (unsigned int)radius);
		XFillArc(dpy, d, gc, x, y, (unsigned int)diameter, (unsigned int)diameter, 90 * 64, 90 * 64);
		XFillArc(dpy, d, gc, x + w - diameter, y, (unsigned int)diameter, (unsigned int)diameter, 0, 90 * 64);
	} else {
		// Square top - inset by radius to respect menu corners
		XFillRectangle(dpy, d, gc, x + radius, y, (unsigned int)(w - diameter), (unsigned int)radius);
	}
	
	// Fill bottom rectangles if rounding bottom
	if (round_bottom) {
		XFillRectangle(dpy, d, gc, x + radius, y + h - radius, (unsigned int)(w - diameter), (unsigned int)radius);
		XFillArc(dpy, d, gc, x, y + h - diameter, (unsigned int)diameter, (unsigned int)diameter, 180 * 64, 90 * 64);
		XFillArc(dpy, d, gc, x + w - diameter, y + h - diameter, (unsigned int)diameter, (unsigned int)diameter, 270 * 64, 90 * 64);
	} else {
		// Square bottom - inset by radius to respect menu corners
		XFillRectangle(dpy, d, gc, x + radius, y + h - radius, (unsigned int)(w - diameter), (unsigned int)radius);
	}
}

static void draw_menu(ContextMenu *m, int can_cut, int can_copy, int can_paste, int can_select_all, int can_clear, int can_undo, int can_redo) {
	if (!m->is_visible || !m->draw) {
		return;
	}
	XSetForeground(m->dpy, m->gc, m->bg);
	XFillRectangle(m->dpy, m->win, m->gc, 0, 0, (unsigned int)MENU_WIDTH, (unsigned int)m->menu_height);
	
	// Draw border only if border_width > 0
	if (m->border_width > 0) {
		XSetForeground(m->dpy, m->gc, m->border);
		XSetLineAttributes(m->dpy, m->gc, (unsigned int)m->border_width, LineSolid, CapButt, JoinMiter);
		int inset = m->border_width / 2;
		draw_rounded_rect(m->dpy, m->win, m->gc, inset, inset, MENU_WIDTH - m->border_width, m->menu_height - m->border_width, m->border_radius);
	}

	// Items start right after border (no menu-level padding)
	int items_top = m->border_width;
	
	// Hover fills from border to border (full width minus borders)
	int hover_x = items_top;
	int hover_width = MENU_WIDTH - (m->border_width * 2) - 1;
	
	for (int i = 0; i < m->menu_item_count && i < 7; i++) {
		// Item position from top (no menu-level padding)
		int y_top = items_top + (i * m->item_height);
		// Text has left padding within the item
		int text_x = m->border_width + 6;
		// Center text vertically: baseline at middle of item height
		int y_text = y_top + (m->item_height + (int)m->font->ascent - (int)m->font->descent) / 2;
		int disabled = (i == 0 && !can_cut) || (i == 1 && !can_copy) || (i == 2 && !can_paste) || (i == 3 && !can_select_all) || (i == 4 && !can_clear) || (i == 5 && !can_undo) || (i == 6 && !can_redo);
		if (!disabled && m->hover_index == i) {
			XSetForeground(m->dpy, m->gc, m->hover_bg);
			// Determine if this is top or bottom item for selective rounding
			int is_first = (i == 0);
			int is_last = (i == m->menu_item_count - 1);
			int round_top = is_first ? 1 : 0;
			int round_bottom = is_last ? 1 : 0;
			// Use border radius if available
			int hover_radius = m->border_radius > 0 ? m->border_radius : 0;
			// Only adjust last item height to not overlap bottom border
			int hover_height = is_last ? m->item_height - 1 : m->item_height;
			fill_rounded_rect_selective(m->dpy, m->win, m->gc, hover_x, y_top, hover_width, hover_height, hover_radius, round_top, round_bottom);
		}
		XftColor *text_color = disabled ? &m->xft_disabled : &m->xft_fg;
		XftDrawStringUtf8(m->draw, text_color, m->font, text_x, y_text, (const FcChar8 *)m->menu_items[i], (int)strlen(m->menu_items[i]));
	}
	
	// Draw separators AFTER menu items so they appear on top of hover
	// Separators: after Paste (index 2), after Select All (index 3), and after Clear (index 4)
	int sep1_y = items_top + (3 * m->item_height); // after item 2 (Paste)
	int sep2_y = items_top + (4 * m->item_height); // after item 3 (Select All)
	int sep3_y = items_top + (5 * m->item_height); // after item 4 (Clear)
	XSetForeground(m->dpy, m->gc, m->border);
	int sep_left = m->border_width + 6;
	int sep_right = MENU_WIDTH - m->border_width - 6;
	XDrawLine(m->dpy, m->win, m->gc, sep_left, sep1_y, sep_right, sep1_y);
	XDrawLine(m->dpy, m->win, m->gc, sep_left, sep2_y, sep_right, sep2_y);
	XDrawLine(m->dpy, m->win, m->gc, sep_left, sep3_y, sep_right, sep3_y);
}

/* Apply rounded corner shape mask to menu window
 * Creates a 1-bit pixmap mask matching the rounded border shape.
 */
static void apply_menu_shape(ContextMenu *m) {
	if (!m) {
		return;
	}
	if (m->border_radius <= 0) {
		// If no radius, remove any shape mask to make window rectangular
		if (m->border_radius == 0) {
			XShapeCombineMask(m->dpy, m->win, ShapeBounding, 0, 0, None, ShapeSet);
		}
		return;
	}
	
	int w = MENU_WIDTH;
	int h = m->menu_height;
	if (h <= 0) {
		return;
	}
	
	// Create a 1-bit pixmap for the shape mask
	Pixmap mask = XCreatePixmap(m->dpy, m->win, (unsigned int)w, (unsigned int)h, 1);
	if (!mask) {
		return;
	}
	GC mask_gc = XCreateGC(m->dpy, mask, 0, NULL);

	// Clear mask to 0 (invisible everywhere)
	XSetForeground(m->dpy, mask_gc, 0);
	XFillRectangle(m->dpy, mask, mask_gc, 0, 0, (unsigned int)w, (unsigned int)h);

	// Draw visible parts with FG=1
	XSetForeground(m->dpy, mask_gc, 1);

	// Fill the rounded rectangle area matching where the border is drawn
	int inset = m->border_width / 2;
	fill_rounded_rect(m->dpy, mask, mask_gc, inset, inset, w - m->border_width, h - m->border_width, m->border_radius);

	// Apply the shape mask to the window
	XShapeCombineMask(m->dpy, m->win, ShapeBounding, 0, 0, mask, ShapeSet);

	XFreeGC(m->dpy, mask_gc);
	XFreePixmap(m->dpy, mask);
}

/* ========== PUBLIC API ========== */

/**
 * @brief Create a new context menu
 *
 * See context.h for full documentation.
 * Initializes context menu window with theme and item configuration.
 */
ContextMenu *menu_create(Display *dpy, int screen, const MiniTheme *theme) {
	ContextMenu *m = calloc(1, sizeof(ContextMenu));
	if (!m) {
		return NULL;
	}
	m->dpy = dpy;
	m->screen = screen;
	m->style = theme->menu;
	m->is_visible = 0;
	m->hover_index = -1;
	m->is_active = 0;

	m->fg = config_color_to_pixel(dpy, screen, theme->menu.fg);
	m->bg = config_color_to_pixel(dpy, screen, theme->menu.bg);
	m->border = config_color_to_pixel(dpy, screen, theme->menu.border);
	m->hover_bg = config_color_to_pixel(dpy, screen, theme->menu.hover_bg);

	// Load font
	m->font = config_open_font(dpy, screen, theme->menu.font_family, theme->menu.font_size);

	// Calculate item height based on font metrics
	// Use proportional padding: max of 8px or 40% of font height
	int font_height = m->font->ascent + m->font->descent;
	int vertical_padding = (font_height * 2) / 5;  // 40% of font height
	if (vertical_padding < 8) vertical_padding = 8;
	m->item_height = font_height + vertical_padding;

	// Setup Xft colors
	XRenderColor xr_fg;
	#define CLAMP_COMP(c) ((c) < 0.0 ? 0 : (c) > 1.0 ? 65535 : (unsigned short)((c) * 65535.0 + 0.5))
	xr_fg.red = CLAMP_COMP(theme->menu.fg.r);
	xr_fg.green = CLAMP_COMP(theme->menu.fg.g);
	xr_fg.blue = CLAMP_COMP(theme->menu.fg.b);
	xr_fg.alpha = CLAMP_COMP(theme->menu.fg.a);
	XftColorAllocValue(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &xr_fg, &m->xft_fg);

	// Setup disabled color (50% mix)
	XRenderColor xr_disabled;
	xr_disabled.red = CLAMP_COMP((theme->menu.bg.r + theme->menu.fg.r) / 2.0);
	xr_disabled.green = CLAMP_COMP((theme->menu.bg.g + theme->menu.fg.g) / 2.0);
	xr_disabled.blue = CLAMP_COMP((theme->menu.bg.b + theme->menu.fg.b) / 2.0);
	xr_disabled.alpha = CLAMP_COMP(1.0);
	#undef CLAMP_COMP
	XftColorAllocValue(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &xr_disabled, &m->xft_disabled);

	m->menu_item_count = 7;
	m->hover_index = -1;
	m->is_active = 0;
	
	// Geometry (hardcoded - context menu is internal UI)
	m->border_width = 1;
	m->border_radius = 4;
	m->padding = 4;

	// Set up fixed item names
	strncpy(m->menu_items[0], "Cut", 31);
	m->menu_items[0][31] = '\0';
	strncpy(m->menu_items[1], "Copy", 31);
	m->menu_items[1][31] = '\0';
	strncpy(m->menu_items[2], "Paste", 31);
	m->menu_items[2][31] = '\0';
	strncpy(m->menu_items[3], "Select All", 31);
	m->menu_items[3][31] = '\0';
	strncpy(m->menu_items[4], "Clear", 31);
	m->menu_items[4][31] = '\0';
	strncpy(m->menu_items[5], "Undo", 31);
	m->menu_items[5][31] = '\0';
	strncpy(m->menu_items[6], "Redo", 31);
	m->menu_items[6][31] = '\0';
	// Calculate menu height: items + border on both sides (separators don't add height)
	m->menu_height = m->menu_item_count * m->item_height + (m->border_width * 2);

	return m;
}

/**
 * @brief Show context menu at position
 *
 * See context.h for full documentation.
 */
void menu_show(ContextMenu *m, int x, int y) {
	if (!m || m->is_visible) {
		return;
	}
	XSetWindowAttributes attr;
	attr.override_redirect = True;
	attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
	                  PointerMotionMask | FocusChangeMask | StructureNotifyMask;

	m->win = XCreateWindow(m->dpy, DefaultRootWindow(m->dpy), x, y, (unsigned int)MENU_WIDTH, (unsigned int)m->menu_height, 1, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWEventMask, &attr);

	m->gc = XCreateGC(m->dpy, m->win, 0, NULL);

	// Create Xft drawing context
	m->draw = XftDrawCreate(m->dpy, m->win, DefaultVisual(m->dpy, m->screen), DefaultColormap(m->dpy, m->screen));

	XMapRaised(m->dpy, m->win);
	
	// Apply rounded corner shape mask if border radius is set
	apply_menu_shape(m);
	
	XSync(m->dpy, False);
	m->is_visible = 1;
	m->hover_index = -1;
	m->is_active = 0;
	XGrabPointer(m->dpy, m->win, False,
	            ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
	            GrabModeAsync, GrabModeAsync,
	            None, None, CurrentTime);
}

/**
 * @brief Hide context menu
 *
 * See context.h for full documentation.
 */
void menu_hide(ContextMenu *m) {
	if (!m || !m->is_visible) {
		return;
	}
	XUngrabPointer(m->dpy, CurrentTime);
	if (m->draw) {
		XftDrawDestroy(m->draw);
		m->draw = NULL;
	}
	XFreeGC(m->dpy, m->gc);
	XDestroyWindow(m->dpy, m->win);
	m->is_visible = 0;
	m->win = 0;
	m->hover_index = -1;
	m->is_active = 0;
}

/**
 * @brief Destroy context menu and free resources
 *
 * See context.h for full documentation.
 */
void menu_destroy(ContextMenu *m) {
	if (!m) {
		return;
	}
	menu_hide(m);
	if (m->font) {
		XftFontClose(m->dpy, m->font);
	}
	XftColorFree(m->dpy, DefaultVisual(m->dpy, m->screen), DefaultColormap(m->dpy, m->screen), &m->xft_fg);
	XftColorFree(m->dpy, DefaultVisual(m->dpy, m->screen), DefaultColormap(m->dpy, m->screen), &m->xft_disabled);
	free(m);
}

// cppcheck-suppress unusedFunction
void menu_draw(ContextMenu *m, int can_cut, int can_copy, int can_paste, int can_select_all, int can_clear, int can_undo, int can_redo) {
	draw_menu(m, can_cut, can_copy, can_paste, can_select_all, can_clear, can_undo, can_redo);
}

static int coords_inside_menu_local(const ContextMenu *m, int x, int y) {
	return x >= 0 && y >= 0 && x < MENU_WIDTH && y < m->menu_height;
}

/**
 * @brief Process X11 events for context menu
 *
 * See context.h for full documentation.
 * Handles hover, clicks, and returns selected item index.
 */
int menu_handle_event(ContextMenu *m, XEvent *ev, int can_cut, int can_copy, int can_paste, int can_select_all, int can_clear, int can_undo, int can_redo) {
	if (!m->is_visible) {
		return -1;
	}
	switch (ev->type) {
		case Expose:
			if (ev->xany.window == m->win) {
				draw_menu(m, can_cut, can_copy, can_paste, can_select_all, can_clear, can_undo, can_redo);
			}
			return -1;
		case FocusOut:
		case UnmapNotify:
			if (ev->xany.window == m->win) {
				menu_hide(m);
			}
			return -1;
		case MotionNotify: {
			if (ev->xany.window != m->win) {
				return -1;
			}
			int items_top = m->border_width;
			int idx = (ev->xmotion.y - items_top) / m->item_height;
			if (idx < 0 || idx >= m->menu_item_count) {
				idx = -1;
			}
			if (m->hover_index != idx) {
				m->hover_index = idx;
				draw_menu(m, can_cut, can_copy, can_paste, can_select_all, can_clear, can_undo, can_redo);
			}
			return -1;
		}
		case ButtonPress:
			if (ev->xany.window != m->win) {
				return -1;
			}
			if (!coords_inside_menu_local(m, ev->xbutton.x, ev->xbutton.y)) {
				menu_hide(m);
				return -1;
			}
			if (ev->xbutton.button == Button1) {
				m->is_active = 1;
				draw_menu(m, can_cut, can_copy, can_paste, can_select_all, can_clear, can_undo, can_redo);
				XFlush(m->dpy);
			}
			return -1;
		case ButtonRelease:
			if (ev->xany.window == m->win && ev->xbutton.button == Button1) {
				int items_top = m->border_width;
				int idx = (ev->xbutton.y - items_top) / m->item_height;
				m->is_active = 0;
				if (coords_inside_menu_local(m, ev->xbutton.x, ev->xbutton.y) &&
				    idx >= 0 && idx < m->menu_item_count) {
					// Check if disabled before returning action
					int disabled = (idx == 0 && !can_cut) || (idx == 1 && !can_copy) ||
					               (idx == 2 && !can_paste) || (idx == 3 && !can_select_all) ||
					               (idx == 4 && !can_clear) || (idx == 5 && !can_undo) || (idx == 6 && !can_redo);
					if (!disabled) {
						menu_hide(m);
						return idx;
					}
				}
				if (m->is_visible) {
					draw_menu(m, can_cut, can_copy, can_paste, can_select_all, can_clear, can_undo, can_redo);
				}
			}
			return -1;
	}
	return -1;
}

int menu_is_visible(ContextMenu *m) {
	return m ? m->is_visible : 0;
}

Window menu_get_window(ContextMenu *m) {
	return m ? m->win : 0;
}
