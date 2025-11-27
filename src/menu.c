/* menu.c - Menu Bar Widget Implementation
 *
 * Implements a horizontal menu bar with dropdown submenus for File, Edit, and About menus.
 * Provides hover highlighting, click detection, and configurable menu items.
 *
 * Features:
 * - Horizontal menu bar with multiple top-level items
 * - Dropdown submenus with configurable items
 * - Mouse hover and click interaction
 * - Themeable appearance via MiniTheme
 * - Dynamic menu item configuration
 * - Proper event handling and focus management
 *
 * This widget provides traditional application menu functionality
 * for X11 applications with minimal dependencies.
 *
 * Internal design notes:
 * - Menubar and submenus are separate override-redirect windows for simplicity.
 * - Layout is computed once per theme update; positions are cached.
 * - Event routing ensures only one submenu is active at a time.
 */

#include "menu.h"
#include "config.h"
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/shape.h>
#include <fontconfig/fontconfig.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MENUBAR_HEIGHT 24
#define SUBMENU_ITEM_HEIGHT 22
#define MENUBAR_ITEM_MIN_WIDTH 92
#define SUBMENU_BORDER_WIDTH 1
#define MENU_ITEM_COUNT 3
#define SUBMENU_TEXT_OFFSET 8

struct MenuBar {
	Display *dpy;
	Window parent;
	Window win;
	Window file_menu_win;
	Window edit_menu_win;
	Window about_menu_win;
	GC gc;
	int screen;

	int x, y, width;
	
	// Geometry properties
	int border_width;
	int border_radius;
	int padding;
	
	int hover_index;
	int is_pressed;
	int active_menu; // 0=none, 1=File, 2=Edit, 3=About
	int active_submenu;
	
	// Drag state
	int is_dragging;
	int drag_start_x;
	int drag_start_y;

	int file_menu_width;
	int file_menu_height;
	int edit_menu_width;
	int edit_menu_height;
	int about_menu_width;
	int about_menu_height;

	unsigned long fg, bg, hover_bg, border;
	MenuBlock style;
	char items[3][32];
	MenuConfig config; // Menu item configuration
	// Xft resources for menubar text
	XftDraw *draw;
	XftFont *font;
	XftColor xft_fg;
	int submenu_item_height; // dynamic height for dropdown items
};

/* ========== RENDERING HELPERS ========== */
/* Color conversion and font loading now centralized in config.c */

/* --- Rounded Rectangle Drawing --- */

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

/* Fill a rounded rectangle with selective left-right corner rounding for menubar hover effects
 * round_left: 1 to round left corners, 0 to square them
 * round_right: 1 to round right corners, 0 to square them
 */
static void fill_rounded_rect_selective_lr(Display *dpy, Drawable d, GC gc, int x, int y, int w, int h, int radius, int round_left, int round_right) {
	if (radius <= 0 || (!round_left && !round_right)) {
		XFillRectangle(dpy, d, gc, x, y, (unsigned int)w, (unsigned int)h);
		return;
	}
	if (radius * 2 > w || radius * 2 > h) {
		XFillRectangle(dpy, d, gc, x, y, (unsigned int)w, (unsigned int)h);
		return;
	}
	
	int diameter = radius * 2;
	
	// Fill the center rectangle
	int fill_x = round_left ? (x + radius) : x;
	int fill_w = w - (round_left ? radius : 0) - (round_right ? radius : 0);
	if (fill_w > 0) {
		XFillRectangle(dpy, d, gc, fill_x, y, (unsigned int)fill_w, (unsigned int)h);
	}
	
	// Fill left rectangles if rounding left
	if (round_left) {
		XFillRectangle(dpy, d, gc, x, y + radius, (unsigned int)radius, (unsigned int)(h - diameter));
		XFillArc(dpy, d, gc, x, y, (unsigned int)diameter, (unsigned int)diameter, 90 * 64, 90 * 64);
		XFillArc(dpy, d, gc, x, y + h - diameter, (unsigned int)diameter, (unsigned int)diameter, 180 * 64, 90 * 64);
	} else {
		// Square left
		XFillRectangle(dpy, d, gc, x, y, (unsigned int)radius, (unsigned int)h);
	}
	
	// Fill right rectangles if rounding right
	if (round_right) {
		XFillRectangle(dpy, d, gc, x + w - radius, y + radius, (unsigned int)radius, (unsigned int)(h - diameter));
		XFillArc(dpy, d, gc, x + w - diameter, y, (unsigned int)diameter, (unsigned int)diameter, 0, 90 * 64);
		XFillArc(dpy, d, gc, x + w - diameter, y + h - diameter, (unsigned int)diameter, (unsigned int)diameter, 270 * 64, 90 * 64);
	} else {
		// Square right
		XFillRectangle(dpy, d, gc, x + w - radius, y, (unsigned int)radius, (unsigned int)h);
	}
}

/* Apply rounded corner shape mask to submenu window */
static void apply_submenu_shape(MenuBar *bar, Window win, int width, int height) {
	if (!bar) {
		return;
	}
	if (bar->border_radius <= 0) {
		// No rounded corners, use simpler rect mask
		if (bar->border_radius == 0) {
			XShapeCombineMask(bar->dpy, win, ShapeBounding, 0, 0, None, ShapeSet);
		}
		return;
	}
	
	if (width <= 0 || height <= 0) {
		return;
	}
	
	// Create a 1-bit pixmap for the shape mask
	Pixmap mask = XCreatePixmap(bar->dpy, win, (unsigned int)width, (unsigned int)height, 1);
	if (!mask) {
		return;
	}
	GC mask_gc = XCreateGC(bar->dpy, mask, 0, NULL);

	// Clear mask to 0 (invisible everywhere)
	XSetForeground(bar->dpy, mask_gc, 0);
	XFillRectangle(bar->dpy, mask, mask_gc, 0, 0, (unsigned int)width, (unsigned int)height);

	// Draw visible parts with FG=1
	XSetForeground(bar->dpy, mask_gc, 1);

	// Fill the entire rounded rectangle area to show full border
	// Shape mask must cover full window so border pixels aren't clipped
	fill_rounded_rect(bar->dpy, mask, mask_gc, 0, 0, width, height, bar->border_radius);

	// Apply the shape mask to the window
	XShapeCombineMask(bar->dpy, win, ShapeBounding, 0, 0, mask, ShapeSet);

	XFreeGC(bar->dpy, mask_gc);
	XFreePixmap(bar->dpy, mask);
}

/* ========== MENUBAR CREATION ========== */

/**
 * @brief Create menubar with custom configuration
 *
 * See menu.h for full documentation.
 * Initializes menubar window with specified menu items and theme.
 */
MenuBar *menubar_create_with_config(Display *dpy, Window parent, const struct MenuBlock *style, int x, int y, int width, int border_width, int border_radius, int padding, const MenuConfig *config) {
	if (!dpy || !style) {
		return NULL;
	}
	if (parent == None) {
		return NULL;
	}
	MenuBar *bar = calloc(1, sizeof(MenuBar));
	if (!bar) {
		return NULL;
	}
	bar->dpy = dpy;
	bar->parent = parent;
	bar->screen = DefaultScreen(dpy);
	bar->x = x;
	bar->y = y;
	bar->width = width;
	bar->border_width = border_width;
	bar->border_radius = border_radius;
	bar->padding = padding;
	bar->style = *style;
	// Copy menu configuration
	if (config) {
		bar->config = *config;
	}
	else {
		// Default empty configuration
		memset(&bar->config, 0, sizeof(MenuConfig));
	}
	bar->fg = config_color_to_pixel(dpy, bar->screen, style->fg);
	bar->bg = config_color_to_pixel(dpy, bar->screen, style->bg);
	bar->hover_bg = config_color_to_pixel(dpy, bar->screen, style->hover_bg);
	bar->border = config_color_to_pixel(dpy, bar->screen, style->border);

	strncpy(bar->items[0], "File", sizeof(bar->items[0]) - 1);
	bar->items[0][sizeof(bar->items[0]) - 1] = '\0';
	strncpy(bar->items[1], "Edit", sizeof(bar->items[1]) - 1);
	bar->items[1][sizeof(bar->items[1]) - 1] = '\0';
	strncpy(bar->items[2], "About", sizeof(bar->items[2]) - 1);
	bar->items[2][sizeof(bar->items[2]) - 1] = '\0';

	bar->hover_index = -1;
	bar->active_menu = 0;
	bar->active_submenu = -1;
	bar->file_menu_width = 0;
	bar->file_menu_height = 0;
	bar->edit_menu_width = 0;
	bar->edit_menu_height = 0;
	bar->about_menu_width = 0;
	bar->about_menu_height = 0;

	XSetWindowAttributes attr;
	attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask;

	bar->win = XCreateWindow(dpy, parent, bar->x, bar->y, (unsigned int)bar->width, (unsigned int)MENUBAR_HEIGHT, 0, CopyFromParent, InputOutput, CopyFromParent, CWEventMask, &attr);

	bar->gc = XCreateGC(dpy, bar->win, 0, NULL);
	bar->file_menu_win = 0;
	bar->edit_menu_win = 0;
	bar->about_menu_win = 0;
	// Initialize Xft for menubar text rendering
	bar->font = config_open_font(dpy, bar->screen, style->font_family, style->font_size);
	bar->draw = XftDrawCreate(dpy, bar->win, DefaultVisual(dpy, bar->screen), DefaultColormap(dpy, bar->screen));
	XRenderColor xr;
	#define CLAMP_COMP(c) ((c) < 0.0 ? 0 : (c) > 1.0 ? 65535 : (unsigned short)((c) * 65535.0 + 0.5))
	xr.red = CLAMP_COMP(style->fg.r);
	xr.green = CLAMP_COMP(style->fg.g);
	xr.blue = CLAMP_COMP(style->fg.b);
	xr.alpha = CLAMP_COMP(style->fg.a);
	#undef CLAMP_COMP
	XftColorAllocValue(dpy, DefaultVisual(dpy, bar->screen), DefaultColormap(dpy, bar->screen), &xr, &bar->xft_fg);
	// Compute dropdown item height from font metrics
	// Use proportional padding: max of 8px or 40% of font height
	int font_height = bar->font->ascent + bar->font->descent;
	int vertical_padding = (font_height * 2) / 5;  // 40% of font height
	if (vertical_padding < 8) vertical_padding = 8;
	bar->submenu_item_height = font_height + vertical_padding;
	XMapWindow(dpy, bar->win);
	return bar;
}

/**
 * @brief Create menubar with default configuration (legacy)
 *
 * See menu.h for full documentation.
 */
MenuBar *menubar_create(Display *dpy, Window parent, const MiniTheme *theme) {
	// Legacy wrapper - for backward compatibility
	MenuConfig default_config = {
		.file_items = { "Exit" },
		.edit_items = { "Configuration", "Reset" },
		.about_items = { "PixelPrism" },
		.file_count = 1,
		.edit_count = 2,
		.about_count = 1
	};
	return menubar_create_with_config(dpy, parent, &theme->menubar, theme->menubar_widget.menubar_x, theme->menubar_widget.menubar_y, theme->menubar_widget.width, theme->menubar_widget.border_width, theme->menubar_widget.border_radius, theme->menubar_widget.padding, &default_config);
}

/**
 * @brief Destroy menubar and free resources
 *
 * See menu.h for full documentation.
 */
void menubar_destroy(MenuBar *bar) {
	if (!bar) {
		return;
	}
	if (bar->file_menu_win) {
		XDestroyWindow(bar->dpy, bar->file_menu_win);
	}
	if (bar->edit_menu_win) {
		XDestroyWindow(bar->dpy, bar->edit_menu_win);
	}
	if (bar->about_menu_win) {
		XDestroyWindow(bar->dpy, bar->about_menu_win);
	}
	if (bar->draw) {
		XftDrawDestroy(bar->draw);
	}
	if (bar->font) {
		XftFontClose(bar->dpy, bar->font);
	}
	XftColorFree(bar->dpy, DefaultVisual(bar->dpy, bar->screen), DefaultColormap(bar->dpy, bar->screen), &bar->xft_fg);
	XFreeGC(bar->dpy, bar->gc);
	free(bar);
}

static void menubar_hide_submenus(MenuBar *bar) {
    if (!bar) {
        return;
    }

    int had_active_menu = bar->active_menu != 0;

    if (bar->file_menu_win) {
        XUnmapWindow(bar->dpy, bar->file_menu_win);
    }
    if (bar->edit_menu_win) {
        XUnmapWindow(bar->dpy, bar->edit_menu_win);
    }
    if (bar->about_menu_win) {
        XUnmapWindow(bar->dpy, bar->about_menu_win);
    }

    if (had_active_menu) {
        XUngrabPointer(bar->dpy, CurrentTime);
    }

    bar->active_menu = 0;
    bar->active_submenu = -1;
}

/**
 * @brief Update menubar theme
 *
 * See menu.h for full documentation.
 */
void menubar_set_theme(MenuBar *bar, const MiniTheme *theme) {
	if (!bar || !theme) {
		return;
	}
	// Update cached style, width, and geometry
	bar->style = theme->menubar;
	bar->width = theme->menubar_widget.width;
	bar->border_width = theme->menubar_widget.border_width;
	bar->border_radius = theme->menubar_widget.border_radius;
	bar->padding = theme->menubar_widget.padding;
	// Update colors
	bar->fg = config_color_to_pixel(bar->dpy, bar->screen, theme->menubar.fg);
	bar->bg = config_color_to_pixel(bar->dpy, bar->screen, theme->menubar.bg);
	bar->hover_bg = config_color_to_pixel(bar->dpy, bar->screen, theme->menubar.hover_bg);
	bar->border = config_color_to_pixel(bar->dpy, bar->screen, theme->menubar.border);
	// Update font
	if (bar->font) {
		XftFontClose(bar->dpy, bar->font);
		bar->font = NULL;
	}
	bar->font = config_open_font(bar->dpy, bar->screen, theme->menubar.font_family, theme->menubar.font_size);
	// Update Xft color
	XftColorFree(bar->dpy, DefaultVisual(bar->dpy, bar->screen), DefaultColormap(bar->dpy, bar->screen), &bar->xft_fg);
	XRenderColor xr;
	#define CLAMP_COMP(c) ((c) < 0.0 ? 0 : (c) > 1.0 ? 65535 : (unsigned short)((c) * 65535.0 + 0.5))
	xr.red = CLAMP_COMP(theme->menubar.fg.r);
	xr.green = CLAMP_COMP(theme->menubar.fg.g);
	xr.blue = CLAMP_COMP(theme->menubar.fg.b);
	xr.alpha = CLAMP_COMP(theme->menubar.fg.a);
	#undef CLAMP_COMP
	XftColorAllocValue(bar->dpy, DefaultVisual(bar->dpy, bar->screen), DefaultColormap(bar->dpy, bar->screen), &xr, &bar->xft_fg);
	// Recompute item height with proportional padding
	int font_height = bar->font->ascent + bar->font->descent;
	int vertical_padding = (font_height * 2) / 5;  // 40% of font height
	if (vertical_padding < 8) vertical_padding = 8;
	bar->submenu_item_height = font_height + vertical_padding;
	// Resize menubar window to new width
	XResizeWindow(bar->dpy, bar->win, (unsigned int)bar->width, (unsigned int)MENUBAR_HEIGHT);
	// Hide any open submenus since their sizes/colors may change
	menubar_hide_submenus(bar);
	bar->hover_index = -1;
	// Redraw
	menubar_draw(bar);
}

void menubar_set_position(MenuBar *bar, int x, int y) {
	if (!bar) {
		return;
	}
	bar->x = x;
	bar->y = y;
	XMoveWindow(bar->dpy, bar->win, x, y);
}

/* ========== DRAWING FUNCTIONS ========== */

static void menubar_draw_submenu(MenuBar *bar, Window win, const char *items[], int count, int width) {
	GC gc = XCreateGC(bar->dpy, win, 0, NULL);
	int menu_h = count * bar->submenu_item_height + (bar->border_width * 2);
	XSetForeground(bar->dpy, gc, bar->bg);
	XFillRectangle(bar->dpy, win, gc, 0, 0, (unsigned int)width, (unsigned int)menu_h);
	
	// Draw border only if border_width > 0
	if (bar->border_width > 0) {
		XSetForeground(bar->dpy, gc, bar->border);
		XSetLineAttributes(bar->dpy, gc, (unsigned int)bar->border_width, LineSolid, CapButt, JoinMiter);
		int inset = bar->border_width / 2;
		draw_rounded_rect(bar->dpy, win, gc, inset, inset, width - bar->border_width, menu_h - bar->border_width, bar->border_radius);
	}

	// Hover fills from border to border (full width minus borders)
	int hover_x = bar->border_width;
	int hover_width = width - (bar->border_width * 2) - 1;
	// Items start right after border (no menu-level padding)
	int items_top = bar->border_width;

	// Xft drawing context for this submenu window
	XftDraw *draw = XftDrawCreate(bar->dpy, win, DefaultVisual(bar->dpy, bar->screen), DefaultColormap(bar->dpy, bar->screen));
	for (int i = 0; i < count; i++) {
		int y_top = items_top + (i * bar->submenu_item_height);
		if (bar->active_submenu == i) {
			XSetForeground(bar->dpy, gc, bar->hover_bg);
			// Determine if this is top or bottom item for selective rounding
			int is_first = (i == 0);
			int is_last = (i == count - 1);
			int round_top = is_first ? 1 : 0;
			int round_bottom = is_last ? 1 : 0;
			// Use border radius if available
			int hover_radius = bar->border_radius > 0 ? bar->border_radius : 0;
			// Only adjust last item height to not overlap bottom border
			int hover_height = is_last ? bar->submenu_item_height - 1 : bar->submenu_item_height;
			fill_rounded_rect_selective(bar->dpy, win, gc, hover_x, y_top, hover_width, hover_height, hover_radius, round_top, round_bottom);
		}
		// Text has left padding within the item
		int text_x = bar->border_width + 6;
		// Center text vertically: baseline at middle of item height
		int text_y = y_top + (bar->submenu_item_height + (int)bar->font->ascent - (int)bar->font->descent) / 2;
		XftDrawStringUtf8(draw, &bar->xft_fg, bar->font, text_x, text_y, (const FcChar8 *)items[i], (int)strlen(items[i]));
	}
	if (draw) {
		XftDrawDestroy(draw);
	}
	XFreeGC(bar->dpy, gc);
}

/* ========== SUBMENU DISPLAY ========== */

static void menubar_show_file_menu(MenuBar *bar) {
	const char **items = bar->config.file_items;
	int count = bar->config.file_count;
	if (count == 0) {
		return; // No items to show
	}
	if (!bar->file_menu_win) {
		// Calculate required width using Xft extents
		// Horizontal padding scales with font size
		int h_padding = bar->font->height;
		if (h_padding < 12) h_padding = 12;
		int max_width = MENUBAR_ITEM_MIN_WIDTH;
		for (int i = 0; i < count; i++) {
			XGlyphInfo ext;
			XftTextExtentsUtf8(bar->dpy, bar->font, (const FcChar8 *)items[i], (int)strlen(items[i]), &ext);
			int item_width = ext.xOff + h_padding + (bar->border_width * 2);
			if (item_width > max_width) {
				max_width = item_width;
			}
		}
		bar->file_menu_width = max_width;
		bar->file_menu_height = count * bar->submenu_item_height + (bar->border_width * 2);

		XSetWindowAttributes attr;
		attr.override_redirect = True;
		attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
		                  PointerMotionMask | FocusChangeMask | StructureNotifyMask;
		attr.background_pixel = bar->bg;

		Window root;
		int rx, ry;
		unsigned int rw, rh, rbw, rd;
		XGetGeometry(bar->dpy, bar->win, &root, &rx, &ry, &rw, &rh, &rbw, &rd);

		int menu_x, menu_y;
		Window child;
		// Drop straight down from left edge of File menu item
		XTranslateCoordinates(bar->dpy, bar->win, root, 0, (int)rh, &menu_x, &menu_y, &child);

		bar->file_menu_win = XCreateWindow(bar->dpy, root, menu_x, menu_y, (unsigned int)bar->file_menu_width, (unsigned int)bar->file_menu_height, 0, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWEventMask, &attr);
		// Apply rounded corner shape mask
		apply_submenu_shape(bar, bar->file_menu_win, bar->file_menu_width, bar->file_menu_height);
	}
	else {
		// Recompute position and move existing window
		Window root;
		int rx, ry;
		unsigned int rw, rh, rbw, rd;
		XGetGeometry(bar->dpy, bar->win, &root, &rx, &ry, &rw, &rh, &rbw, &rd);
		int menu_x, menu_y;
		Window child;
		XTranslateCoordinates(bar->dpy, bar->win, root, 0, (int)rh, &menu_x, &menu_y, &child);
		XMoveWindow(bar->dpy, bar->file_menu_win, menu_x, menu_y);
	}
	XMapRaised(bar->dpy, bar->file_menu_win);
	bar->active_menu = 1;
	bar->active_submenu = -1;

	// Grab focus to detect clicks outside the menu via FocusOut
	XSetInputFocus(bar->dpy, bar->file_menu_win, RevertToParent, CurrentTime);
	XGrabPointer(bar->dpy, bar->file_menu_win, False,
	            ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
	            GrabModeAsync, GrabModeAsync,
	            None, None, CurrentTime);

	menubar_draw_submenu(bar, bar->file_menu_win, items, count, bar->file_menu_width);
}

static void menubar_show_edit_menu(MenuBar *bar) {
	const char **items = bar->config.edit_items;
	int count = bar->config.edit_count;
	if (count == 0) {
		return; // No items to show
	}
	if (!bar->edit_menu_win) {
		// Calculate required width
		// Horizontal padding scales with font size
		int h_padding = bar->font->height;
		if (h_padding < 12) h_padding = 12;
		int max_width = MENUBAR_ITEM_MIN_WIDTH;
		for (int i = 0; i < count; i++) {
			XGlyphInfo ext;
			XftTextExtentsUtf8(bar->dpy, bar->font, (const FcChar8 *)items[i], (int)strlen(items[i]), &ext);
			int item_width = ext.xOff + h_padding + (bar->border_width * 2);
			if (item_width > max_width) {
				max_width = item_width;
			}
		}
		bar->edit_menu_width = max_width;
		bar->edit_menu_height = count * bar->submenu_item_height + (bar->border_width * 2);

		XSetWindowAttributes attr;
		attr.override_redirect = True;
		attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
		                  PointerMotionMask | FocusChangeMask | StructureNotifyMask;
		// Set background to None to prevent automatic clearing during resize
		// This is essential for flicker-free resizing - prevents X server from clearing background
		attr.background_pixmap = None;
		attr.bit_gravity = NorthWestGravity; // Preserve content on resize

		Window root;
		int rx, ry;
		unsigned int rw, rh, rbw, rd;
		XGetGeometry(bar->dpy, bar->win, &root, &rx, &ry, &rw, &rh, &rbw, &rd);

		// Compute exact segment start (inner split) to align dropdown
		int inner_x = 1;
		int inner_w = bar->width - 2;
		int base_w = inner_w / 3;
		int rem = inner_w % 3;
		int seg0 = base_w + (rem > 0 ? 1 : 0);
		// Edit starts at end of first segment
		int seg_start_x = inner_x + seg0;
		int menu_x, menu_y;
		Window child;
		XTranslateCoordinates(bar->dpy, bar->win, root, seg_start_x, (int)rh, &menu_x, &menu_y, &child);

		bar->edit_menu_win = XCreateWindow(bar->dpy, root, menu_x, menu_y, (unsigned int)bar->edit_menu_width, (unsigned int)bar->edit_menu_height, 0, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWEventMask, &attr);
		// Apply rounded corner shape mask
		apply_submenu_shape(bar, bar->edit_menu_win, bar->edit_menu_width, bar->edit_menu_height);
	}
	else {
		// Recompute position and move existing window
		Window root;
		int rx, ry;
		unsigned int rw, rh, rbw, rd;
		XGetGeometry(bar->dpy, bar->win, &root, &rx, &ry, &rw, &rh, &rbw, &rd);
		int inner_x = 1;
		int inner_w = bar->width - 2;
		int base_w = inner_w / 3;
		int rem = inner_w % 3;
		int seg0 = base_w + (rem > 0 ? 1 : 0);
		int seg_start_x = inner_x + seg0;
		int menu_x, menu_y;
		Window child;
		XTranslateCoordinates(bar->dpy, bar->win, root, seg_start_x, (int)rh, &menu_x, &menu_y, &child);
		XMoveWindow(bar->dpy, bar->edit_menu_win, menu_x, menu_y);
	}
	XMapRaised(bar->dpy, bar->edit_menu_win);
	bar->active_menu = 2;
	bar->active_submenu = -1;

	// Grab focus to detect clicks outside the menu via FocusOut
	XSetInputFocus(bar->dpy, bar->edit_menu_win, RevertToParent, CurrentTime);
	XGrabPointer(bar->dpy, bar->edit_menu_win, False,
	            ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
	            GrabModeAsync, GrabModeAsync,
	            None, None, CurrentTime);

	menubar_draw_submenu(bar, bar->edit_menu_win, items, count, bar->edit_menu_width);
}

static void menubar_show_about_menu(MenuBar *bar) {
	const char **items = bar->config.about_items;
	int count = bar->config.about_count;
	if (count == 0) {
		return; // No items to show
	}
	if (!bar->about_menu_win) {
		// Calculate required width
		// Horizontal padding scales with font size
		int h_padding = bar->font->height;
		if (h_padding < 12) h_padding = 12;
		int max_width = MENUBAR_ITEM_MIN_WIDTH;
		for (int i = 0; i < count; i++) {
			XGlyphInfo ext;
			XftTextExtentsUtf8(bar->dpy, bar->font, (const FcChar8 *)items[i], (int)strlen(items[i]), &ext);
			int item_width = ext.xOff + h_padding + (bar->border_width * 2);
			if (item_width > max_width) {
				max_width = item_width;
			}
		}
		bar->about_menu_width = max_width;
		bar->about_menu_height = count * bar->submenu_item_height + (bar->border_width * 2);

		XSetWindowAttributes attr;
		attr.override_redirect = True;
		attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
		                  PointerMotionMask | FocusChangeMask | StructureNotifyMask;
		// Set background to None to prevent automatic clearing during resize
		// This is essential for flicker-free resizing - prevents X server from clearing background
		attr.background_pixmap = None;
		attr.bit_gravity = NorthWestGravity; // Preserve content on resize

		Window root;
		int rx, ry;
		unsigned int rw, rh, rbw, rd;
		XGetGeometry(bar->dpy, bar->win, &root, &rx, &ry, &rw, &rh, &rbw, &rd);

		// Compute exact segment start (inner split) to align dropdown
		int inner_x = 1;
		int inner_w = bar->width - 2;
		int base_w = inner_w / 3;
		int rem = inner_w % 3;
		int seg0 = base_w + (rem > 0 ? 1 : 0);
		int seg1 = base_w + (rem > 1 ? 1 : 0);
		// About starts at end of first+second segments
		int seg_start_x = inner_x + seg0 + seg1;
		int menu_x, menu_y;
		Window child;
		XTranslateCoordinates(bar->dpy, bar->win, root, seg_start_x, (int)rh, &menu_x, &menu_y, &child);

		bar->about_menu_win = XCreateWindow(bar->dpy, root, menu_x, menu_y, (unsigned int)bar->about_menu_width, (unsigned int)bar->about_menu_height, 0, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWEventMask, &attr);
		// Apply rounded corner shape mask
		apply_submenu_shape(bar, bar->about_menu_win, bar->about_menu_width, bar->about_menu_height);
	}
	else {
		// Recompute position and move existing window
		Window root;
		int rx, ry;
		unsigned int rw, rh, rbw, rd;
		XGetGeometry(bar->dpy, bar->win, &root, &rx, &ry, &rw, &rh, &rbw, &rd);
		int inner_x = 1;
		int inner_w = bar->width - 2;
		int base_w = inner_w / 3;
		int rem = inner_w % 3;
		int seg0 = base_w + (rem > 0 ? 1 : 0);
		int seg1 = base_w + (rem > 1 ? 1 : 0);
		int seg_start_x = inner_x + seg0 + seg1;
		int menu_x, menu_y;
		Window child;
		XTranslateCoordinates(bar->dpy, bar->win, root, seg_start_x, (int)rh, &menu_x, &menu_y, &child);
		XMoveWindow(bar->dpy, bar->about_menu_win, menu_x, menu_y);
	}
	XMapRaised(bar->dpy, bar->about_menu_win);
	bar->active_menu = 3;
	bar->active_submenu = -1;

	// Grab focus to detect clicks outside the menu via FocusOut
	XSetInputFocus(bar->dpy, bar->about_menu_win, RevertToParent, CurrentTime);
	XGrabPointer(bar->dpy, bar->about_menu_win, False,
	            ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
	            GrabModeAsync, GrabModeAsync,
	            None, None, CurrentTime);

	menubar_draw_submenu(bar, bar->about_menu_win, items, count, bar->about_menu_width);
}

static void menubar_draw_internal(MenuBar *bar) {
	XSetForeground(bar->dpy, bar->gc, bar->bg);
	XFillRectangle(bar->dpy, bar->win, bar->gc, 0, 0, (unsigned int)bar->width, (unsigned int)MENUBAR_HEIGHT);
	
	// Draw border only if border_width > 0
	if (bar->border_width > 0) {
		XSetForeground(bar->dpy, bar->gc, bar->border);
		XSetLineAttributes(bar->dpy, bar->gc, (unsigned int)bar->border_width, LineSolid, CapButt, JoinMiter);
		int inset = bar->border_width / 2;
		draw_rounded_rect(bar->dpy, bar->win, bar->gc, inset, inset, bar->width - bar->border_width, MENUBAR_HEIGHT - bar->border_width, bar->border_radius);
	}

	// Compute inner area excluding border (1px if present, 0 otherwise)
	int inner_x = bar->border_width > 0 ? 1 : 0;
	// Prevent underflow if width is too small
	int border_offset = bar->border_width > 0 ? 2 : 0;
	if (bar->width < border_offset + 2) {
		return; // Too small to draw meaningful menubar
	}
	int inner_w = bar->width - border_offset;
	int base_w = inner_w / MENU_ITEM_COUNT;
	int rem = inner_w % MENU_ITEM_COUNT;
	int seg_w[MENU_ITEM_COUNT];
	seg_w[0] = base_w + (rem > 0 ? 1 : 0);
	seg_w[1] = base_w + (rem > 1 ? 1 : 0);
	seg_w[2] = base_w;

	int seg_x = inner_x;
	for (int i = 0; i < MENU_ITEM_COUNT; i++) {
		if (bar->hover_index == i || bar->active_menu == i + 1) {
			XSetForeground(bar->dpy, bar->gc, bar->hover_bg);
			int fill_y = bar->border_width > 0 ? 1 : 0;
			int fill_h = bar->border_width > 0 ? MENUBAR_HEIGHT - 3 : MENUBAR_HEIGHT;
			int fill_w = seg_w[i] - 1; // Reduce width by 1 to not overlap right edge
			
			// Determine if this is first or last item for selective rounding
			int is_first = (i == 0);
			int is_last = (i == MENU_ITEM_COUNT - 1);
			int round_left = is_first ? 1 : 0;
			int round_right = is_last ? 1 : 0;
			
			// Use border radius if available
			int hover_radius = bar->border_radius > 0 ? bar->border_radius : 0;
			
			fill_rounded_rect_selective_lr(bar->dpy, bar->win, bar->gc, seg_x, fill_y, fill_w, fill_h, hover_radius, round_left, round_right);
		}
		// Use Xft to center and render text within this segment
		XGlyphInfo ext;
		XftTextExtentsUtf8(bar->dpy, bar->font, (const FcChar8 *)bar->items[i], (int)strlen(bar->items[i]), &ext);
		int text_x = seg_x + (seg_w[i] - ext.xOff) / 2;
		int baseline = (MENUBAR_HEIGHT + bar->font->ascent - bar->font->descent) / 2;
		XftDrawStringUtf8(bar->draw, &bar->xft_fg, bar->font, text_x, baseline, (const FcChar8 *)bar->items[i], (int)strlen(bar->items[i]));
		seg_x += seg_w[i];
	}
	// Draw vertical borders between menu items at exact boundaries (only if border enabled)
	if (bar->border_width > 0) {
		XSetForeground(bar->dpy, bar->gc, bar->border);
		seg_x = inner_x + seg_w[0];
		XDrawLine(bar->dpy, bar->win, bar->gc, seg_x, 0, seg_x, MENUBAR_HEIGHT - 1);
		seg_x += seg_w[1];
		XDrawLine(bar->dpy, bar->win, bar->gc, seg_x, 0, seg_x, MENUBAR_HEIGHT - 1);
	}
}

void menubar_draw(MenuBar *bar) {
	if (!bar) {
		return;
	}
	menubar_draw_internal(bar);
	XFlush(bar->dpy);
}

void menubar_hide_all_submenus(MenuBar *bar) {
	if (!bar) {
		return;
	}
	menubar_hide_submenus(bar);
	bar->hover_index = -1;
	menubar_draw_internal(bar);
	XFlush(bar->dpy);
}

/* ========== EVENT HANDLING ========== */

/**
 * @brief Process X11 events for menubar
 *
 * See menu.h for full documentation.
 * Handles mouse hover, clicks, keyboard navigation, and returns menu item ID when selected.
 */
int menubar_handle_event(MenuBar *bar, XEvent *ev) {
	if (!bar) {
		return -1;
	}
	
	// Auto-hide menus when user clicks or types outside menu system
	if ((ev->type == ButtonPress || ev->type == KeyPress) &&
	    !menubar_is_menubar_window(bar, ev->xany.window)) {
		if (bar->active_menu > 0) {
			menubar_hide_all_submenus(bar);
		}
		return -1; // Event not for us, but we processed the auto-hide
	}
	
	// Handle submenu events
	if (bar->active_menu > 0) {
		Window target_win = (bar->active_menu == 1) ? bar->file_menu_win :
		                    (bar->active_menu == 2) ? bar->edit_menu_win :
		                    (bar->active_menu == 3) ? bar->about_menu_win : 0;
		// While a submenu is open, detect pointer moving over the menubar itself and switch menus
		if (ev->type == MotionNotify) {
			Window root = DefaultRootWindow(bar->dpy);
			int mb_rx = 0, mb_ry = 0;
			Window child;
			XTranslateCoordinates(bar->dpy, bar->win, root, 0, 0, &mb_rx, &mb_ry, &child);
			int x_root = ev->xmotion.x_root;
			int y_root = ev->xmotion.y_root;
			if (x_root >= mb_rx && x_root < mb_rx + bar->width &&
			    y_root >= mb_ry && y_root < mb_ry + MENUBAR_HEIGHT) {
				int rel_x = x_root - mb_rx;
				int idx = rel_x / (bar->width / 3);
				if (idx < 0) {
					idx = 0;
				}
				else if (idx > 2) {
					idx = 2;
				}
				if ((idx + 1) != bar->active_menu) {
					menubar_hide_submenus(bar);
					if (idx == 0) {
						menubar_show_file_menu(bar);
					}
					else if (idx == 1) {
						menubar_show_edit_menu(bar);
					}
					else if (idx == 2) {
						menubar_show_about_menu(bar);
					}
					bar->hover_index = idx;
					menubar_draw_internal(bar);
					return -1;
				}
			}
		}
		// Hide on focus loss
		if (ev->type == FocusOut || ev->type == UnmapNotify) {
			if (ev->xany.window == target_win) {
				menubar_hide_submenus(bar);
				bar->hover_index = -1;
				menubar_draw_internal(bar);
				return -1;
			}
		}
		if (ev->type == MotionNotify && ev->xany.window == target_win) {
			int items_top = bar->border_width;
			int idx = (ev->xmotion.y - items_top) / bar->submenu_item_height;
			int max_items = (bar->active_menu == 1) ? bar->config.file_count :
			                (bar->active_menu == 2) ? bar->config.edit_count :
			                (bar->active_menu == 3) ? bar->config.about_count : 0;
			if (idx < 0 || idx >= max_items) {
				idx = -1;
			}
			if (bar->active_submenu != idx) {
				bar->active_submenu = idx;
				if (bar->active_menu == 1) {
					menubar_draw_submenu(bar, bar->file_menu_win, bar->config.file_items, bar->config.file_count, bar->file_menu_width);
				}
				else if (bar->active_menu == 2) {
					menubar_draw_submenu(bar, bar->edit_menu_win, bar->config.edit_items, bar->config.edit_count, bar->edit_menu_width);
				}
				else if (bar->active_menu == 3) {
					menubar_draw_submenu(bar, bar->about_menu_win, bar->config.about_items, bar->config.about_count, bar->about_menu_width);
				}
			}
			return -1;
		}
		// Handle right-click anywhere while submenu is open: hide dropdown
		if (ev->type == ButtonPress && ev->xbutton.button == Button3) {
			menubar_hide_submenus(bar);
			bar->hover_index = -1;
			menubar_draw_internal(bar);
			return -1;
		}
		// Handle ButtonPress - check if outside menu bounds
		if (ev->type == ButtonPress && ev->xbutton.button == Button1) {
			// Get menu dimensions
			int menu_height = (bar->active_menu == 1) ? bar->file_menu_height :
			                  (bar->active_menu == 2) ? bar->edit_menu_height :
			                  (bar->active_menu == 3) ? bar->about_menu_height : 0;
			int menu_width = (bar->active_menu == 1) ? bar->file_menu_width :
			                 (bar->active_menu == 2) ? bar->edit_menu_width :
			                 (bar->active_menu == 3) ? bar->about_menu_width : 0;
			// Check if click is outside the submenu
			if (ev->xany.window == target_win) {
				if (ev->xbutton.x < 0 || ev->xbutton.x >= menu_width ||
				    ev->xbutton.y < 0 || ev->xbutton.y >= menu_height) {
					menubar_hide_submenus(bar);
					bar->hover_index = -1;
					menubar_draw_internal(bar);
					return -1;
				}
			}
			else if (ev->xany.window != bar->win) {
				// Click on a window that's not menubar or submenu
				menubar_hide_submenus(bar);
				bar->hover_index = -1;
				menubar_draw_internal(bar);
				return -1;
			}
		}
		if (ev->type == ButtonRelease && ev->xbutton.button == Button1 && ev->xany.window == target_win) {
			int items_top = bar->border_width;
			int idx = (ev->xbutton.y - items_top) / bar->submenu_item_height;
			int max_items = (bar->active_menu == 1) ? bar->config.file_count :
			                (bar->active_menu == 2) ? bar->config.edit_count :
			                (bar->active_menu == 3) ? bar->config.about_count : 0;

			// Validate click is within bounds
			int menu_height = (bar->active_menu == 1) ? bar->file_menu_height :
			                  (bar->active_menu == 2) ? bar->edit_menu_height :
			                  (bar->active_menu == 3) ? bar->about_menu_height : 0;
			int menu_width = (bar->active_menu == 1) ? bar->file_menu_width :
			                 (bar->active_menu == 2) ? bar->edit_menu_width :
			                 (bar->active_menu == 3) ? bar->about_menu_width : 0;
			if (ev->xbutton.x >= 0 && ev->xbutton.x < menu_width &&
			    ev->xbutton.y >= 0 && ev->xbutton.y < menu_height &&
			    idx >= 0 && idx < max_items) {
				// Save active_menu before hiding (which resets it to 0)
				int active = bar->active_menu;

				menubar_hide_submenus(bar);
				bar->hover_index = -1;
				menubar_draw_internal(bar);
				// Return configurable action codes
				if (active == 1) {
					return idx; // File items: 0, 1, 2, ...
				}
				if (active == 2) {
					return 100 + idx; // Edit items: 100, 101, 102, ...
				}
				if (active == 3) {
					return 200 + idx; // About items: 200, 201, 202, ...
				}
			}
			return -1;
		}
		if (ev->type == Expose && ev->xany.window == target_win) {
			if (bar->active_menu == 1) {
				menubar_draw_submenu(bar, bar->file_menu_win, bar->config.file_items, bar->config.file_count, bar->file_menu_width);
			}
			else if (bar->active_menu == 2) {
				menubar_draw_submenu(bar, bar->edit_menu_win, bar->config.edit_items, bar->config.edit_count, bar->edit_menu_width);
			}
			else if (bar->active_menu == 3) {
				menubar_draw_submenu(bar, bar->about_menu_win, bar->config.about_items, bar->config.about_count, bar->about_menu_width);
			}
			return -1;
		}
	}
	// Handle main menubar events
	if (ev->xany.window != bar->win) {
		return -1;
	}
	switch (ev->type) {
		case Expose:
			menubar_draw_internal(bar);
			return -1;
		case MotionNotify: {
			// Handle dragging
			if (bar->is_dragging) {
				int dx = ev->xmotion.x_root - bar->drag_start_x;
				int dy = ev->xmotion.y_root - bar->drag_start_y;
				bar->x += dx;
				bar->y += dy;
				XMoveWindow(bar->dpy, bar->win, bar->x, bar->y);
				bar->drag_start_x = ev->xmotion.x_root;
				bar->drag_start_y = ev->xmotion.y_root;
				return -1;
			}
			
			int item_w = bar->width / MENU_ITEM_COUNT;
			int idx = ev->xmotion.x / item_w;
			if (idx < 0 || idx >= MENU_ITEM_COUNT) {
				idx = -1;
			}
			// If a menu is already open, hovering across items should switch dropdowns
			if (bar->active_menu > 0 && idx >= 0 && idx < MENU_ITEM_COUNT && (idx + 1) != bar->active_menu) {
				menubar_hide_submenus(bar);
				if (idx == 0) {
					menubar_show_file_menu(bar);
				}
				else if (idx == 1) {
					menubar_show_edit_menu(bar);
				}
				else {
					menubar_show_about_menu(bar);
				}
				bar->hover_index = idx;
				menubar_draw_internal(bar);
				return -1;
			}
			if (idx != bar->hover_index) {
				bar->hover_index = idx;
				menubar_draw_internal(bar);
			}
			return -1;
		}
		case LeaveNotify:
			bar->hover_index = -1;
			menubar_draw_internal(bar);
			return -1;
		case ButtonPress:
			if (ev->xbutton.button == Button1) {
				int item_w = bar->width / MENU_ITEM_COUNT;
				int idx = ev->xbutton.x / item_w;
				// Bounds check to prevent array out-of-bounds
				if (idx >= 0 && idx < MENU_ITEM_COUNT) {
					if (idx == 0) {
						menubar_hide_submenus(bar);
						menubar_show_file_menu(bar);
						menubar_draw_internal(bar);
					}
					else if (idx == 1) {
						menubar_hide_submenus(bar);
						menubar_show_edit_menu(bar);
						menubar_draw_internal(bar);
					}
					else {
						menubar_hide_submenus(bar);
						menubar_show_about_menu(bar);
						menubar_draw_internal(bar);
					}
				}
			}
			else if (ev->xbutton.button == Button2 || ev->xbutton.button == Button3) {
				// Start dragging with middle or right button
				bar->is_dragging = 1;
				bar->drag_start_x = ev->xbutton.x_root;
				bar->drag_start_y = ev->xbutton.y_root;
			}
			return -1;
		case ButtonRelease:
			if (ev->xbutton.button == Button2 || ev->xbutton.button == Button3) {
				bar->is_dragging = 0;
			}
			return -1;
		default:
		break;
	}
	return -1;
}

// cppcheck-suppress unusedFunction
Window menubar_get_window(const MenuBar *bar) {
	return bar ? bar->win : None;
}

int menubar_is_menubar_window(const MenuBar *bar, Window win) {
	if (!bar || win == None) {
		return 0;
	}
	if (win == bar->win) {
		return 1;
	}
	if (win == bar->file_menu_win) {
		return 1;
	}
	if (win == bar->edit_menu_win) {
		return 1;
	}
	if (win == bar->about_menu_win) {
		return 1;
	}
	return 0;
}


/* ========== CONFIGURATION MANAGEMENT ========== */

void menu_config_init_defaults(struct MenuBlock *block, const char *menu_type) {
	(void)menu_type; // Unused - kept for API compatibility
	strncpy(block->font_family, "DejaVu Sans", sizeof(block->font_family) - 1);
	block->font_size = 14;
	block->fg = (ConfigColor){0.180f, 0.204f, 0.212f, 1.0f}; // #2E3436 dark slate
	block->bg = (ConfigColor){1.0f, 1.0f, 1.0f, 1.0f}; // #FFFFFF white
	block->border = (ConfigColor){0.804f, 0.780f, 0.761f, 1.0f}; // #CDC7C2 light gray
	block->hover_bg = (ConfigColor){0.882f, 0.871f, 0.859f, 1.0f}; // #E1DEDB light hover
}

// cppcheck-suppress unusedFunction
void menu_config_parse(struct MenuBlock *block, const char *key, const char *value) {
	if (strcmp(key, "font") == 0 || strcmp(key, "font-family") == 0) {
		strncpy(block->font_family, value, sizeof(block->font_family) - 1);
	}
	else if (strcmp(key, "font-size") == 0) {
		block->font_size = atoi(value);
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
	else if (strcmp(key, "hover-background") == 0) {
		unsigned int r = 0, g = 0, b = 0;
		if (sscanf(value, "#%02x%02x%02x", &r, &g, &b) == 3) {
			block->hover_bg.r = (float)r / 255.0f;
			block->hover_bg.g = (float)g / 255.0f;
			block->hover_bg.b = (float)b / 255.0f;
			block->hover_bg.a = 1.0f;
		}
	}
}

void menu_config_write(FILE *f, const struct MenuBlock *block, const char *menu_type) {
	fprintf(f, "[%s]\n", menu_type);
	fprintf(f, "active-background = #%02X%02X%02X\n",
		(int)(block->active_bg.r * 255),
		(int)(block->active_bg.g * 255),
		(int)(block->active_bg.b * 255));
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
	fprintf(f, "font-family = %s\n", block->font_family);
	fprintf(f, "font-size = %d\n", block->font_size);
	fprintf(f, "hover-background = #%02X%02X%02X\n\n",
		(int)(block->hover_bg.r * 255),
		(int)(block->hover_bg.g * 255),
		(int)(block->hover_bg.b * 255));
}

/* ========== MENUBAR WIDGET GEOMETRY (menubar-widget section) ========== */

void menubar_widget_config_init_defaults(Config *cfg) {
	if (!cfg) {
		return;
	}
	cfg->menubar_widget.menubar_x = 306;
	cfg->menubar_widget.menubar_y = 0;
	cfg->menubar_widget.width = 278;
	cfg->menubar_widget.border_width = 1;
	cfg->menubar_widget.border_radius = 4;
	cfg->menubar_widget.padding = 4;
}

void menubar_widget_config_parse(Config *cfg, const char *key, const char *value) {
	if (!cfg || !key || !value) {
		return;
	}
	
	// Alphabetized geometry keys only
	if (strcmp(key, "border-radius") == 0) {
		cfg->menubar_widget.border_radius = atoi(value);
	}
	else if (strcmp(key, "border-width") == 0) {
		cfg->menubar_widget.border_width = atoi(value);
	}
	else if (strcmp(key, "menubar-x") == 0) {
		cfg->menubar_widget.menubar_x = atoi(value);
	}
	else if (strcmp(key, "menubar-y") == 0) {
		cfg->menubar_widget.menubar_y = atoi(value);
	}
	else if (strcmp(key, "padding") == 0) {
		cfg->menubar_widget.padding = atoi(value);
	}
	else if (strcmp(key, "width") == 0) {
		cfg->menubar_widget.width = atoi(value);
	}
}

void menubar_widget_config_write(FILE *f, const Config *cfg) {
	if (!f || !cfg) {
		return;
	}
	
	// Alphabetized geometry keys only
	fprintf(f, "[menubar-widget]\n");
	fprintf(f, "border-radius = %d\n", cfg->menubar_widget.border_radius);
	fprintf(f, "border-width = %d\n", cfg->menubar_widget.border_width);
	fprintf(f, "menubar-x = %d\n", cfg->menubar_widget.menubar_x);
	fprintf(f, "menubar-y = %d\n", cfg->menubar_widget.menubar_y);
	fprintf(f, "padding = %d\n", cfg->menubar_widget.padding);
	fprintf(f, "width = %d\n\n", cfg->menubar_widget.width);
}
