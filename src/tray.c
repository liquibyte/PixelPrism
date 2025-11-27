/* tray.c - System Tray Icon Implementation
 *
 * Implements freedesktop.org system tray specification for X11.
 * Handles XEMBED protocol and tray icon embedding.
 *
 * Internal design notes:
 * - Watches for _NET_SYSTEM_TRAY_Sn selection owners and re-docks when needed.
 * - Uses a small custom menu via context.c for tray interactions.
 * - Embedding logic is isolated so main window code only toggles visibility.
 */

#include "tray.h"
#include "context.h"
#include "config.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/xpm.h>
#include <X11/extensions/shape.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

/* XEMBED protocol opcodes */
#define XEMBED_EMBEDDED_NOTIFY 0
#define XEMBED_WINDOW_ACTIVATE 1
#define XEMBED_WINDOW_DEACTIVATE 2
#define XEMBED_REQUEST_FOCUS 3
#define XEMBED_FOCUS_IN 4
#define XEMBED_FOCUS_OUT 5
#define XEMBED_FOCUS_NEXT 6
#define XEMBED_FOCUS_PREV 7
#define XEMBED_MODALITY_ON 10
#define XEMBED_MODALITY_OFF 11
#define XEMBED_REGISTER_ACCELERATOR 12
#define XEMBED_UNREGISTER_ACCELERATOR 13
#define XEMBED_ACTIVATE_ACCELERATOR 14

/* System tray opcodes */
#define SYSTEM_TRAY_REQUEST_DOCK 0
/* #define SYSTEM_TRAY_BEGIN_MESSAGE 1 */ /* Unused opcode, kept for reference */
/* #define SYSTEM_TRAY_CANCEL_MESSAGE 2 */ /* Unused opcode, kept for reference */

/* XEMBED info flags */
#define XEMBED_MAPPED (1 << 0)

/* Menu layout constants */
#define MENU_ITEM_HEIGHT 24

/* ========== RENDERING HELPERS ========== */

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

/* Safe font opening with proper pattern cleanup
 * Prevents fontconfig pattern leaks by explicitly managing pattern lifecycle.
 */
static XftFont *open_font(Display *dpy, int screen, const char *fam, int size) {
	FcPattern *pat = FcNameParse((const FcChar8 *)(fam ? fam : "sans"));
	if (!pat) {
		return XftFontOpenName(dpy, screen, "sans-14");
	}
	FcPatternAddInteger(pat, FC_PIXEL_SIZE, size);
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	FcResult result;
	FcPattern *match = FcFontMatch(NULL, pat, &result);
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

/* Tray context structure */
struct TrayContext {
	Display *dpy;
	int screen;
	Window tray_icon;
	Window main_window;  // Reference to main app window for visibility checking
	Window tray_manager;
	Window menu_window;
	Atom xa_tray_selection;
	Atom xa_tray_opcode;
	Atom xa_xembed;
	Atom xa_xembed_info;
	Pixmap icon_pixmap;
	Pixmap icon_mask;
	int icon_width;
	int icon_height;
	int menu_visible;
	int menu_x;
	int menu_y;
	int menu_width;
	int menu_height;
	int menu_item_height;  // Dynamic item height based on font
	XftFont *menu_font;
	XftDraw *menu_draw;
	XftColor menu_fg;
	XftColor menu_bg;
	XftColor menu_hover_bg;
	int menu_hover;
	TrayMenuBlock theme;
	Time last_button_time;
};

/* ========== INTERNAL HELPERS ========== */

/**
 * find_system_tray - Locate the system tray manager window
 * @dpy Display connection
 * @screen Screen number
 * @xa_tray_selection Tray selection atom
 *
 * Return: Tray manager window or None if not found
 */
static Window find_system_tray(Display *dpy, int screen, Atom xa_tray_selection) {
	Window tray_owner = XGetSelectionOwner(dpy, xa_tray_selection);
	// If selection owner exists, use it
	if (tray_owner != None) {
		return tray_owner;
	}
	// FluxBox fallback: try root window as some versions don't advertise properly
	return RootWindow(dpy, screen);
}

/**
 * send_tray_message - Send message to system tray manager
 * @dpy Display connection
 * @tray_manager Tray manager window
 * @message Message opcode
 * @data1: First data parameter
 * @data2: Second data parameter
 * @data3: Third data parameter
 */
static void send_tray_message(Display *dpy, Window tray_manager, long message, long data1, long data2, long data3) {
	XEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.xclient.type = ClientMessage;
	ev.xclient.window = tray_manager;
	ev.xclient.message_type = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = (long)CurrentTime;
	ev.xclient.data.l[1] = message;
	ev.xclient.data.l[2] = data1;
	ev.xclient.data.l[3] = data2;
	ev.xclient.data.l[4] = data3;
	XSendEvent(dpy, tray_manager, False, NoEventMask, &ev);
	XSync(dpy, False);
}

/**
 * set_xembed_info - Set XEMBED_INFO property on tray icon
 * @ctx Tray context
 */
static void set_xembed_info(TrayContext *ctx) {
	unsigned long info[2];
	info[0] = 0; // XEMBED protocol version
	info[1] = XEMBED_MAPPED; // Mapped flag
	XChangeProperty(ctx->dpy, ctx->tray_icon, ctx->xa_xembed_info, ctx->xa_xembed_info, 32, PropModeReplace, (unsigned char *)info, 2);
}

/**
 * create_context_menu - Create the popup context menu window
 * @ctx Tray context
 */
static void create_context_menu(TrayContext *ctx) {
	XSetWindowAttributes attrs;
	Colormap colormap;
	XRenderColor xrc_fg, xrc_bg, xrc_hover;
	XColor xc_bg = {0}, xc_border = {0};

	// Load font from theme first to calculate item height
	colormap = DefaultColormap(ctx->dpy, ctx->screen);
	ctx->menu_font = open_font(ctx->dpy, ctx->screen, ctx->theme.font_family, ctx->theme.font_size);

	// Calculate dynamic item height based on font metrics
	// Use proportional padding: max of 8px or 40% of font height
	int font_height = ctx->menu_font->ascent + ctx->menu_font->descent;
	int vertical_padding = (font_height * 2) / 5;  // 40% of font height
	if (vertical_padding < 8) vertical_padding = 8;
	ctx->menu_item_height = font_height + vertical_padding;

	// Menu dimensions - 4 items + separator + border on both sides (no menu-level padding)
	ctx->menu_width = 150;
	ctx->menu_height = (ctx->theme.border_width * 2) + (ctx->menu_item_height * 4) + 5; // border + 4 items + 5px separator

	// Convert theme colors to X pixels
	xc_bg.red = (unsigned short)(ctx->theme.bg.r * 65535);
	xc_bg.green = (unsigned short)(ctx->theme.bg.g * 65535);
	xc_bg.blue = (unsigned short)(ctx->theme.bg.b * 65535);
	xc_bg.flags = DoRed | DoGreen | DoBlue;
	XAllocColor(ctx->dpy, colormap, &xc_bg);

	xc_border.red = (unsigned short)(ctx->theme.border.r * 65535);
	xc_border.green = (unsigned short)(ctx->theme.border.g * 65535);
	xc_border.blue = (unsigned short)(ctx->theme.border.b * 65535);
	xc_border.flags = DoRed | DoGreen | DoBlue;
	XAllocColor(ctx->dpy, colormap, &xc_border);

	// Create menu window
	attrs.override_redirect = True;
	attrs.event_mask = ExposureMask |
	                   ButtonPressMask | ButtonReleaseMask |
	                   PointerMotionMask |
	                   FocusChangeMask | StructureNotifyMask |
	                   LeaveWindowMask;
	attrs.background_pixel = xc_bg.pixel;
	attrs.border_pixel = xc_border.pixel;

	ctx->menu_window = XCreateWindow(ctx->dpy, RootWindow(ctx->dpy, ctx->screen), 0, 0, (unsigned int)ctx->menu_width, (unsigned int)ctx->menu_height, 1, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWEventMask |
		CWBackPixel | CWBorderPixel, &attrs);

	// Create Xft draw context
	ctx->menu_draw = XftDrawCreate(ctx->dpy, ctx->menu_window, DefaultVisual(ctx->dpy, ctx->screen), colormap);

	// Setup colors from theme
	xrc_fg.red = (unsigned short)(ctx->theme.fg.r * 65535);
	xrc_fg.green = (unsigned short)(ctx->theme.fg.g * 65535);
	xrc_fg.blue = (unsigned short)(ctx->theme.fg.b * 65535);
	xrc_fg.alpha = (unsigned short)(ctx->theme.fg.a * 65535);
	XftColorAllocValue(ctx->dpy, DefaultVisual(ctx->dpy, ctx->screen), colormap, &xrc_fg, &ctx->menu_fg);

	xrc_bg.red = (unsigned short)(ctx->theme.bg.r * 65535);
	xrc_bg.green = (unsigned short)(ctx->theme.bg.g * 65535);
	xrc_bg.blue = (unsigned short)(ctx->theme.bg.b * 65535);
	xrc_bg.alpha = (unsigned short)(ctx->theme.bg.a * 65535);
	XftColorAllocValue(ctx->dpy, DefaultVisual(ctx->dpy, ctx->screen), colormap, &xrc_bg, &ctx->menu_bg);

	xrc_hover.red = (unsigned short)(ctx->theme.hover_bg.r * 65535);
	xrc_hover.green = (unsigned short)(ctx->theme.hover_bg.g * 65535);
	xrc_hover.blue = (unsigned short)(ctx->theme.hover_bg.b * 65535);
	xrc_hover.alpha = (unsigned short)(ctx->theme.hover_bg.a * 65535);
	XftColorAllocValue(ctx->dpy, DefaultVisual(ctx->dpy, ctx->screen), colormap, &xrc_hover, &ctx->menu_hover_bg);

	ctx->menu_visible = 0;
	ctx->menu_hover = 0;
}

/**
 * apply_menu_shape - Apply rounded corner shape mask to menu window
 * @ctx Tray context
 */
static void apply_menu_shape(TrayContext *ctx) {
	if (!ctx) {
		return;
	}
	if (ctx->theme.border_radius <= 0) {
		// If no radius, remove any shape mask to make window rectangular
		if (ctx->theme.border_radius == 0) {
			XShapeCombineMask(ctx->dpy, ctx->menu_window, ShapeBounding, 0, 0, None, ShapeSet);
		}
		return;
	}
	
	int w = ctx->menu_width;
	int h = ctx->menu_height;
	if (w <= 0 || h <= 0) {
		return;
	}
	
	// Create a 1-bit pixmap for the shape mask
	Pixmap mask = XCreatePixmap(ctx->dpy, ctx->menu_window, (unsigned int)w, (unsigned int)h, 1);
	if (!mask) {
		return;
	}
	GC mask_gc = XCreateGC(ctx->dpy, mask, 0, NULL);

	// Clear mask to 0 (invisible everywhere)
	XSetForeground(ctx->dpy, mask_gc, 0);
	XFillRectangle(ctx->dpy, mask, mask_gc, 0, 0, (unsigned int)w, (unsigned int)h);

	// Draw visible parts with FG=1
	XSetForeground(ctx->dpy, mask_gc, 1);

	// Fill the entire rounded rectangle area to show full border
	// Shape mask must cover full window so border pixels aren't clipped
	fill_rounded_rect(ctx->dpy, mask, mask_gc, 0, 0, w, h, ctx->theme.border_radius);

	// Apply the shape mask to the window
	XShapeCombineMask(ctx->dpy, ctx->menu_window, ShapeBounding, 0, 0, mask, ShapeSet);

	XFreeGC(ctx->dpy, mask_gc);
	XFreePixmap(ctx->dpy, mask);
}

/**
 * draw_context_menu - Redraw the context menu
 * @ctx Tray context
 */
static void draw_context_menu(TrayContext *ctx) {
	// Check if main window is visible to determine menu text
	const char *window_action = "Show Window";
	if (ctx->main_window) {
		XWindowAttributes attrs;
		if (XGetWindowAttributes(ctx->dpy, ctx->main_window, &attrs)) {
			if (attrs.map_state == IsViewable) {
				window_action = "Minimize";
			}
			else {
				window_action = "Maximize";
			}
		}
	}
	
	const char *items[] = {
		"Pick Color", window_action, "Copy as Hex", "Exit"
	};
	// Items start right after border (no menu-level padding)
	int items_top = ctx->theme.border_width;
	int separator_y = items_top + ctx->menu_item_height * 3 + 2;
	XColor xc_bg = {0}, xc_hover = {0}, xc_border = {0};
	Colormap cmap = DefaultColormap(ctx->dpy, ctx->screen);

	// Convert theme colors
	xc_bg.red = (unsigned short)(ctx->theme.bg.r * 65535);
	xc_bg.green = (unsigned short)(ctx->theme.bg.g * 65535);
	xc_bg.blue = (unsigned short)(ctx->theme.bg.b * 65535);
	xc_bg.flags = DoRed | DoGreen | DoBlue;
	XAllocColor(ctx->dpy, cmap, &xc_bg);

	xc_hover.red = (unsigned short)(ctx->theme.hover_bg.r * 65535);
	xc_hover.green = (unsigned short)(ctx->theme.hover_bg.g * 65535);
	xc_hover.blue = (unsigned short)(ctx->theme.hover_bg.b * 65535);
	xc_hover.flags = DoRed | DoGreen | DoBlue;
	XAllocColor(ctx->dpy, cmap, &xc_hover);

	xc_border.red = (unsigned short)(ctx->theme.border.r * 65535);
	xc_border.green = (unsigned short)(ctx->theme.border.g * 65535);
	xc_border.blue = (unsigned short)(ctx->theme.border.b * 65535);
	xc_border.flags = DoRed | DoGreen | DoBlue;
	XAllocColor(ctx->dpy, cmap, &xc_border);

	// Clear background
	XSetForeground(ctx->dpy, DefaultGC(ctx->dpy, ctx->screen), xc_bg.pixel);
	XFillRectangle(ctx->dpy, ctx->menu_window, DefaultGC(ctx->dpy, ctx->screen), 0, 0, (unsigned int)ctx->menu_width, (unsigned int)ctx->menu_height);
	
	// Draw border only if border_width > 0
	if (ctx->theme.border_width > 0) {
		GC gc = XCreateGC(ctx->dpy, ctx->menu_window, 0, NULL);
		XSetForeground(ctx->dpy, gc, xc_border.pixel);
		XSetLineAttributes(ctx->dpy, gc, (unsigned int)ctx->theme.border_width, LineSolid, CapButt, JoinMiter);
		int inset = ctx->theme.border_width / 2;
		draw_rounded_rect(ctx->dpy, ctx->menu_window, gc, inset, inset, ctx->menu_width - ctx->theme.border_width, ctx->menu_height - ctx->theme.border_width, ctx->theme.border_radius);
		XFreeGC(ctx->dpy, gc);
	}
	
	// Hover fills from border to border (full width minus borders)
	int hover_x = ctx->theme.border_width;
	int hover_width = ctx->menu_width - (ctx->theme.border_width * 2) - 1;
	
	// Draw each menu item
	for (int i = 0; i < 4; i++) {
		int y_pos = items_top + (i * ctx->menu_item_height);
		// Adjust for separator
		if (i == 3) {
			y_pos += 5;
		}
		// Highlight if hovering
		if (ctx->menu_hover == i + 1) {
			XSetForeground(ctx->dpy, DefaultGC(ctx->dpy, ctx->screen), xc_hover.pixel);
			// Determine if this is top or bottom item for selective rounding
			int is_first = (i == 0);
			int is_last = (i == 3); // Last visual item (Exit)
			int round_top = is_first ? 1 : 0;
			int round_bottom = is_last ? 1 : 0;
			// Use border radius if available
			int hover_radius = ctx->theme.border_radius > 0 ? ctx->theme.border_radius : 0;
			// Only adjust last item height to not overlap bottom border
			int hover_height = is_last ? ctx->menu_item_height - 1 : ctx->menu_item_height;
			fill_rounded_rect_selective(ctx->dpy, ctx->menu_window, DefaultGC(ctx->dpy, ctx->screen), hover_x, y_pos, hover_width, hover_height, hover_radius, round_top, round_bottom);
		}
		// Draw text with left padding within the item
		int text_x = ctx->theme.border_width + 6;
		// Center text vertically: baseline at middle of item height
		int text_y = y_pos + (ctx->menu_item_height + (int)ctx->menu_font->ascent - (int)ctx->menu_font->descent) / 2;

		XftDrawString8(ctx->menu_draw, &ctx->menu_fg, ctx->menu_font, text_x, text_y, (const FcChar8 *)items[i], (int)strlen(items[i]));
	}
	// Draw separator line
	XSetForeground(ctx->dpy, DefaultGC(ctx->dpy, ctx->screen), xc_border.pixel);
	XDrawLine(ctx->dpy, ctx->menu_window, DefaultGC(ctx->dpy, ctx->screen), 5, separator_y, ctx->menu_width - 5, separator_y);
}

/**
 * show_context_menu - Show context menu at position
 * @ctx Tray context
 * @x Screen X coordinate
 * @y Screen Y coordinate
 */
static void show_context_menu(TrayContext *ctx, int x, int y) {
	// Get screen dimensions
	int screen_width = DisplayWidth(ctx->dpy, ctx->screen);
	int screen_height = DisplayHeight(ctx->dpy, ctx->screen);

	// Position menu so bottom-right aligns with icon's top-left (menu appears above icon)
	ctx->menu_x = x - ctx->menu_width;
	ctx->menu_y = y - ctx->menu_height;
	// Check right edge
	if (ctx->menu_x + ctx->menu_width > screen_width) {
		ctx->menu_x = screen_width - ctx->menu_width;
	}
	// Check bottom edge
	if (ctx->menu_y + ctx->menu_height > screen_height) {
		ctx->menu_y = screen_height - ctx->menu_height;
	}
	// Ensure it's not off the left/top edges
	if (ctx->menu_x < 0) {
		ctx->menu_x = 0;
	}
	if (ctx->menu_y < 0) {
		ctx->menu_y = 0;
	}
	XMoveWindow(ctx->dpy, ctx->menu_window, ctx->menu_x, ctx->menu_y);
	XMapRaised(ctx->dpy, ctx->menu_window);
	// Apply rounded corner shape mask before grabbing the pointer, mirroring
	// the behavior of the generic text context menu.
	apply_menu_shape(ctx);
	// Ensure the menu window is viewable before grabbing the pointer so that
	// clicks anywhere (including on other windows/desktop) are redirected here.
	XSync(ctx->dpy, False);
	// Give the menu keyboard focus so a click on another window will
	// generate FocusOut, matching the behavior of menubar dropdowns.
	XSetInputFocus(ctx->dpy, ctx->menu_window, RevertToParent, ctx->last_button_time);
	ctx->menu_visible = 1;
	ctx->menu_hover = 0;
	int grab_result = XGrabPointer(ctx->dpy, ctx->menu_window, False,
	            ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
	            GrabModeAsync, GrabModeAsync,
	            None, None, ctx->last_button_time);
	if (grab_result != GrabSuccess) {
		XUngrabPointer(ctx->dpy, CurrentTime);
		XSync(ctx->dpy, False);
		grab_result = XGrabPointer(ctx->dpy, ctx->menu_window, False,
		            ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
		            GrabModeAsync, GrabModeAsync,
		            None, None, ctx->last_button_time);
	}
	draw_context_menu(ctx);
	XFlush(ctx->dpy);
}

/**
 * hide_context_menu - Hide the context menu
 * @ctx Tray context
 */
static void hide_context_menu(TrayContext *ctx) {
	if (ctx->menu_visible) {
		XUngrabPointer(ctx->dpy, CurrentTime);
		XUnmapWindow(ctx->dpy, ctx->menu_window);
		ctx->menu_visible = 0;
		XFlush(ctx->dpy);
	}
}

/* ========== PUBLIC API ========== */

/**
 * @brief Check if system tray is available
 *
 * See tray.h for full documentation.
 */
int tray_is_available(Display *dpy, int screen) {
	char atom_name[32];
	snprintf(atom_name, sizeof(atom_name), "_NET_SYSTEM_TRAY_S%d", screen);
	Atom xa_tray = XInternAtom(dpy, atom_name, False);
	return XGetSelectionOwner(dpy, xa_tray) != None ? 1 : 0;
}

/**
 * @brief Create system tray icon
 *
 * See tray.h for full documentation.
 * Embeds icon into system tray using XEMBED protocol.
 */
TrayContext *tray_create(Display *dpy, int screen, const char **icon_xpm, const void *menu_theme, Window main_window) {
	TrayContext *ctx;
	char atom_name[32];
	XSetWindowAttributes attrs;
	XpmAttributes xpm_attrs;
	int status;

	// Check if tray is available - but continue anyway for FluxBox compatibility
	// FluxBox 1.3.x may not properly advertise the selection owner but still works
	(void)tray_is_available(dpy, screen);

	// Allocate context
	ctx = (TrayContext *)calloc(1, sizeof(TrayContext));
	if (!ctx) {
		return NULL;
	}
	ctx->dpy = dpy;
	ctx->screen = screen;
	ctx->main_window = main_window;
	// Copy theme or use defaults
	if (menu_theme) {
		ctx->theme = *(const TrayMenuBlock *)menu_theme;
	}
	else {
		// Default theme
		strncpy(ctx->theme.font_family, "sans", sizeof(ctx->theme.font_family) - 1);
		ctx->theme.font_size = 12;
		ctx->theme.fg = (ConfigColor) {
			1.0, 1.0, 1.0, 1.0
		};
		ctx->theme.bg = (ConfigColor) {
			0.16, 0.16, 0.16, 1.0
		};
		ctx->theme.hover_bg = (ConfigColor) {
			0.25, 0.25, 0.25, 1.0
		};
		ctx->theme.border = (ConfigColor) {
			0.25, 0.25, 0.25, 1.0
		};
		ctx->theme.border_width = 1;
	}
	// Get atoms
	snprintf(atom_name, sizeof(atom_name), "_NET_SYSTEM_TRAY_S%d", screen);
	ctx->xa_tray_selection = XInternAtom(dpy, atom_name, False);
	ctx->xa_tray_opcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
	ctx->xa_xembed = XInternAtom(dpy, "_XEMBED", False);
	ctx->xa_xembed_info = XInternAtom(dpy, "_XEMBED_INFO", False);

	// Find tray manager
	ctx->tray_manager = find_system_tray(dpy, screen, ctx->xa_tray_selection);
	if (ctx->tray_manager == None) {
		free(ctx);
		return NULL;
	}
	// Load icon from XPM
	xpm_attrs.valuemask = 0;
	status = XpmCreatePixmapFromData(dpy, RootWindow(dpy, screen), (char **)icon_xpm, &ctx->icon_pixmap, &ctx->icon_mask, &xpm_attrs);
	if (status != XpmSuccess) {
		fprintf(stderr, "Failed to create tray icon pixmap: %d\n", status);
		free(ctx);
		return NULL;
	}
	
	// Validate the created pixmap immediately
	if (ctx->icon_pixmap == None) {
		fprintf(stderr, "Tray icon pixmap is None after creation\n");
		if (ctx->icon_mask != None) {
			XFreePixmap(dpy, ctx->icon_mask);
		}
		free(ctx);
		return NULL;
	}
	
	// Test pixmap validity by checking geometry
	unsigned int width, height, depth, border_width;
	Window root_return;
	int x_return, y_return;
	if (!XGetGeometry(dpy, ctx->icon_pixmap, &root_return, &x_return, &y_return, 
	                 &width, &height, &border_width, &depth)) {
		fprintf(stderr, "Invalid tray icon pixmap geometry\n");
		if (ctx->icon_pixmap != None) {
			XFreePixmap(dpy, ctx->icon_pixmap);
		}
		if (ctx->icon_mask != None) {
			XFreePixmap(dpy, ctx->icon_mask);
		}
		free(ctx);
		return NULL;
	}
	
	//fprintf(stderr, "Tray icon pixmap created successfully: %ux%ux%u\n", width, height, depth);
	
	ctx->icon_width = (int)xpm_attrs.width;
	ctx->icon_height = (int)xpm_attrs.height;
	XpmFreeAttributes(&xpm_attrs);

	// Create tray icon window
	attrs.event_mask = ButtonPressMask | ButtonReleaseMask | ExposureMask | StructureNotifyMask;
	attrs.override_redirect = True;

	ctx->tray_icon = XCreateWindow(dpy, RootWindow(dpy, screen), -1, -1, (unsigned int)ctx->icon_width, (unsigned int)ctx->icon_height, 0, CopyFromParent, InputOutput, CopyFromParent, CWEventMask | CWOverrideRedirect, &attrs);

	// Set XEMBED info
	set_xembed_info(ctx);

	// Map tray icon window
	XMapWindow(dpy, ctx->tray_icon);
	XSync(dpy, False);

	// Tray managers need time to process window map before dock request
	usleep(25000);

	// Send XEMBED dock request
	send_tray_message(dpy, ctx->tray_manager, SYSTEM_TRAY_REQUEST_DOCK, (long)ctx->tray_icon, 0, 0);

	XSync(dpy, False);

	// Create context menu
	create_context_menu(ctx);

	return ctx;
}

/**
 * @brief Destroy tray icon and free resources
 *
 * See tray.h for full documentation.
 */
void tray_destroy(TrayContext *ctx) {
	if (!ctx) {
		return;
	}
	// CRITICAL: Free Xft resources BEFORE destroying windows
	// XftColorFree must be called before XDestroyWindow to avoid RenderBadPicture errors
	Colormap colormap = DefaultColormap(ctx->dpy, ctx->screen);
	XftColorFree(ctx->dpy, DefaultVisual(ctx->dpy, ctx->screen), colormap, &ctx->menu_fg);
	XftColorFree(ctx->dpy, DefaultVisual(ctx->dpy, ctx->screen), colormap, &ctx->menu_bg);
	XftColorFree(ctx->dpy, DefaultVisual(ctx->dpy, ctx->screen), colormap, &ctx->menu_hover_bg);
	
	if (ctx->menu_draw) {
		XftDrawDestroy(ctx->menu_draw);
	}
	if (ctx->menu_font) {
		XftFontClose(ctx->dpy, ctx->menu_font);
	}
	// Now safe to destroy windows after Xft resources are freed
	if (ctx->menu_window) {
		XDestroyWindow(ctx->dpy, ctx->menu_window);
	}
	if (ctx->icon_pixmap) {
		XFreePixmap(ctx->dpy, ctx->icon_pixmap);
	}
	if (ctx->icon_mask) {
		XFreePixmap(ctx->dpy, ctx->icon_mask);
	}
	if (ctx->tray_icon) {
		XDestroyWindow(ctx->dpy, ctx->tray_icon);
	}
	free(ctx);
}

/**
 * @brief Process X11 events for tray icon
 *
 * See tray.h for full documentation.
 * Returns 1 when tray icon is clicked.
 */
int tray_handle_event(TrayContext *ctx, XEvent *event) {
	if (!ctx) {
		return 0;
	}
	// Handle menu window events
	if (event->xany.window == ctx->menu_window) {
		switch (event->type) {
			case ButtonPress:
				/* Any click outside the menu bounds should hide the menu */
				if (event->xbutton.x < 0 || event->xbutton.y < 0 ||
				    event->xbutton.x >= ctx->menu_width ||
				    event->xbutton.y >= ctx->menu_height) {
					hide_context_menu(ctx);
					return 0;
				}
				if (event->xbutton.button == Button1) {
					int clicked_item = 0;
					int items_top = ctx->theme.border_width;
					int y = event->xbutton.y - items_top;
					// Determine which item was clicked
					if (y >= 0 && y < ctx->menu_item_height) {
						clicked_item = 1; // Pick Color
					}
					else if (y < ctx->menu_item_height * 2) {
						clicked_item = 2; // Show Window
					}
					else if (y < ctx->menu_item_height * 3) {
						clicked_item = 3; // Copy as Hex
					}
					else if (y >= ctx->menu_item_height * 3 + 5) {
						clicked_item = 4; // Exit (after separator)
					}
					hide_context_menu(ctx);
					if (clicked_item == 1) {
						return 2; // Pick Color
					}
					if (clicked_item == 2) {
						return 3; // Show Window
					}
					if (clicked_item == 3) {
						return 4; // Copy as Hex
					}
					if (clicked_item == 4) {
						return 5; // Exit
					}
				}
				else if (event->xbutton.button == Button3) {
					/* Right-click anywhere while menu is visible just closes it */
					hide_context_menu(ctx);
				}
			break;

			case MotionNotify: {
				// Determine hover item
				int items_top = ctx->theme.border_width;
				int y = event->xmotion.y - items_top;
				int old_hover = ctx->menu_hover;
				if (y >= 0 && y < ctx->menu_item_height) {
					ctx->menu_hover = 1; // Pick Color
				}
				else if (y < ctx->menu_item_height * 2) {
					ctx->menu_hover = 2; // Show Window
				}
				else if (y < ctx->menu_item_height * 3) {
					ctx->menu_hover = 3; // Copy as Hex
				}
				else if (y >= ctx->menu_item_height * 3 + 5) {
					ctx->menu_hover = 4; // Exit
				}
				else {
					ctx->menu_hover = 0; // Separator area
				}
				if (old_hover != ctx->menu_hover) {
					draw_context_menu(ctx);
				}
				break;
			}

			case LeaveNotify:
				// Remove hover effect
				ctx->menu_hover = 0;
				draw_context_menu(ctx);
			break;

			case UnmapNotify:
				// If the menu window is unmapped externally, keep state in sync
				hide_context_menu(ctx);
			break;

			case Expose:
				draw_context_menu(ctx);
			break;

			case FocusOut:
				// Hide menu when it loses focus
				hide_context_menu(ctx);
			break;

			default:
			break;
		}
		return 0;
	}
	// Check if event is for our tray icon
	if (event->xany.window != ctx->tray_icon) {
		// Click outside menu - hide it
		if (ctx->menu_visible && event->type == ButtonPress) {
			hide_context_menu(ctx);
		}
		return 0;
	}
	switch (event->type) {
		case ButtonPress:
			if (event->xbutton.button == Button1) {
				// Left click - toggle window immediately on press
				ctx->last_button_time = event->xbutton.time;
				hide_context_menu(ctx);
				return 1;
			}
			// For Button3 (right click), defer showing the menu until ButtonRelease
		break;

		case ButtonRelease:
			if (event->xbutton.button == Button3) {
				// Right click release - show context menu above icon
				ctx->last_button_time = event->xbutton.time;
				// Get icon window position and translate to root coordinates
				XWindowAttributes icon_attrs;
				XGetWindowAttributes(ctx->dpy, ctx->tray_icon, &icon_attrs);
				int icon_x, icon_y;
				Window child;
				XTranslateCoordinates(ctx->dpy, ctx->tray_icon, RootWindow(ctx->dpy, ctx->screen), 0, 0, &icon_x, &icon_y, &child);
				// Pass icon's top-right position so menu opens aligned to the right edge
				show_context_menu(ctx, icon_x + ctx->icon_width, icon_y);
				return 0;
			}
		break;

		case Expose:
			// Redraw icon manually
			if (ctx->icon_pixmap && ctx->tray_icon) {
				// Validate pixmap before using it
				unsigned int width, height, depth, border_width;
				Window root_return;
				int x_return, y_return;
				if (XGetGeometry(ctx->dpy, ctx->icon_pixmap, &root_return, &x_return, &y_return, 
				                 &width, &height, &border_width, &depth)) {
					GC icon_gc = XCreateGC(ctx->dpy, ctx->tray_icon, 0, NULL);
					// Copy pixmap to window
					XCopyArea(ctx->dpy, ctx->icon_pixmap, ctx->tray_icon, icon_gc,
					          0, 0, (unsigned int)ctx->icon_width, (unsigned int)ctx->icon_height,
					          0, 0);
					XFreeGC(ctx->dpy, icon_gc);
				} else {
					fprintf(stderr, "Warning: Tray icon pixmap became invalid, skipping redraw\n");
				}
			}
		break;

		case ReparentNotify:
			// Icon embedded in tray
		break;

		case DestroyNotify:
			// Tray was destroyed
		break;

		default:
		break;
	}
	return 0;
}

// cppcheck-suppress unusedFunction
Window tray_get_window(TrayContext *ctx) {
	return ctx ? ctx->tray_icon : None;
}

void tray_set_theme(TrayContext *ctx, const MiniTheme *theme) {
	if (!ctx || !theme) {
		return;
	}
	
	// Update theme - copy each field
	strncpy(ctx->theme.font_family, theme->tray_menu.font_family, sizeof(ctx->theme.font_family) - 1);
	ctx->theme.font_family[sizeof(ctx->theme.font_family) - 1] = '\0';
	ctx->theme.font_size = theme->tray_menu.font_size;
	ctx->theme.fg = theme->tray_menu.fg;
	ctx->theme.bg = theme->tray_menu.bg;
	ctx->theme.hover_bg = theme->tray_menu.hover_bg;
	ctx->theme.border = theme->tray_menu.border;
	ctx->theme.border_width = theme->tray_menu_widget.border_width;
	ctx->theme.border_radius = theme->tray_menu_widget.border_radius;
	
	// Update font
	if (ctx->menu_font) {
		XftFontClose(ctx->dpy, ctx->menu_font);
	}
	Colormap colormap = DefaultColormap(ctx->dpy, ctx->screen);
	ctx->menu_font = open_font(ctx->dpy, ctx->screen, ctx->theme.font_family, ctx->theme.font_size);
	
	// Recalculate item height based on new font
	int font_height = ctx->menu_font->ascent + ctx->menu_font->descent;
	int vertical_padding = (font_height * 2) / 5;
	if (vertical_padding < 8) vertical_padding = 8;
	ctx->menu_item_height = font_height + vertical_padding;
	
	// Recalculate menu dimensions
	ctx->menu_height = (ctx->theme.border_width * 2) + (ctx->menu_item_height * 4) + 5;
	
	// Resize menu window
	XResizeWindow(ctx->dpy, ctx->menu_window, (unsigned int)ctx->menu_width, (unsigned int)ctx->menu_height);
	
	// Update colors
	XftColorFree(ctx->dpy, DefaultVisual(ctx->dpy, ctx->screen), colormap, &ctx->menu_fg);
	XftColorFree(ctx->dpy, DefaultVisual(ctx->dpy, ctx->screen), colormap, &ctx->menu_bg);
	XftColorFree(ctx->dpy, DefaultVisual(ctx->dpy, ctx->screen), colormap, &ctx->menu_hover_bg);
	
	XRenderColor xrc;
	xrc.red = (unsigned short)(ctx->theme.fg.r * 65535);
	xrc.green = (unsigned short)(ctx->theme.fg.g * 65535);
	xrc.blue = (unsigned short)(ctx->theme.fg.b * 65535);
	xrc.alpha = (unsigned short)(ctx->theme.fg.a * 65535);
	XftColorAllocValue(ctx->dpy, DefaultVisual(ctx->dpy, ctx->screen), colormap, &xrc, &ctx->menu_fg);
	
	xrc.red = (unsigned short)(ctx->theme.bg.r * 65535);
	xrc.green = (unsigned short)(ctx->theme.bg.g * 65535);
	xrc.blue = (unsigned short)(ctx->theme.bg.b * 65535);
	xrc.alpha = (unsigned short)(ctx->theme.bg.a * 65535);
	XftColorAllocValue(ctx->dpy, DefaultVisual(ctx->dpy, ctx->screen), colormap, &xrc, &ctx->menu_bg);
	
	xrc.red = (unsigned short)(ctx->theme.hover_bg.r * 65535);
	xrc.green = (unsigned short)(ctx->theme.hover_bg.g * 65535);
	xrc.blue = (unsigned short)(ctx->theme.hover_bg.b * 65535);
	xrc.alpha = (unsigned short)(ctx->theme.hover_bg.a * 65535);
	XftColorAllocValue(ctx->dpy, DefaultVisual(ctx->dpy, ctx->screen), colormap, &xrc, &ctx->menu_hover_bg);
	
	// Reapply shape mask with new dimensions
	apply_menu_shape(ctx);
	
	// Redraw if visible
	if (ctx->menu_visible) {
		draw_context_menu(ctx);
	}
}


/* ========== CONFIGURATION MANAGEMENT ========== */

// cppcheck-suppress unusedFunction
void tray_config_init_defaults(Config *cfg) {
	strncpy(cfg->tray_menu.font_family, "DejaVu Sans", sizeof(cfg->tray_menu.font_family) - 1);
	cfg->tray_menu.font_size = 14;
	cfg->tray_menu.fg = (ConfigColor){0.180f, 0.204f, 0.212f, 1.0f}; // #2E3436 dark slate
	cfg->tray_menu.bg = (ConfigColor){1.0f, 1.0f, 1.0f, 1.0f}; // #FFFFFF white
	cfg->tray_menu.hover_bg = (ConfigColor){0.882f, 0.871f, 0.859f, 1.0f}; // #E1DEDB light hover
	cfg->tray_menu.border = (ConfigColor){0.804f, 0.780f, 0.761f, 1.0f}; // #CDC7C2 light gray
	cfg->tray_menu_widget.border_width = 1;
	cfg->tray_menu_widget.border_radius = 4;
}

// cppcheck-suppress unusedFunction
void tray_config_parse(Config *cfg, const char *key, const char *value) {
	if (strcmp(key, "font-family") == 0) {
		strncpy(cfg->tray_menu.font_family, value, sizeof(cfg->tray_menu.font_family) - 1);
	}
	else if (strcmp(key, "font-size") == 0) {
		cfg->tray_menu.font_size = atoi(value);
	}
	else if (strcmp(key, "color") == 0) {
		unsigned int r = 0, g = 0, b = 0;
		if (sscanf(value, "#%02x%02x%02x", &r, &g, &b) == 3) {
			cfg->tray_menu.fg.r = (float)r / 255.0f;
			cfg->tray_menu.fg.g = (float)g / 255.0f;
			cfg->tray_menu.fg.b = (float)b / 255.0f;
			cfg->tray_menu.fg.a = 1.0f;
		}
	}
	else if (strcmp(key, "background") == 0) {
		unsigned int r = 0, g = 0, b = 0;
		if (sscanf(value, "#%02x%02x%02x", &r, &g, &b) == 3) {
			cfg->tray_menu.bg.r = (float)r / 255.0f;
			cfg->tray_menu.bg.g = (float)g / 255.0f;
			cfg->tray_menu.bg.b = (float)b / 255.0f;
			cfg->tray_menu.bg.a = 1.0f;
		}
	}
	else if (strcmp(key, "hover-background") == 0) {
		unsigned int r = 0, g = 0, b = 0;
		if (sscanf(value, "#%02x%02x%02x", &r, &g, &b) == 3) {
			cfg->tray_menu.hover_bg.r = (float)r / 255.0f;
			cfg->tray_menu.hover_bg.g = (float)g / 255.0f;
			cfg->tray_menu.hover_bg.b = (float)b / 255.0f;
			cfg->tray_menu.hover_bg.a = 1.0f;
		}
	}
	else if (strcmp(key, "border") == 0) {
		unsigned int r = 0, g = 0, b = 0;
		if (sscanf(value, "#%02x%02x%02x", &r, &g, &b) == 3) {
			cfg->tray_menu.border.r = (float)r / 255.0f;
			cfg->tray_menu.border.g = (float)g / 255.0f;
			cfg->tray_menu.border.b = (float)b / 255.0f;
			cfg->tray_menu.border.a = 1.0f;
		}
	}
	else if (strcmp(key, "border-width") == 0) {
		cfg->tray_menu_widget.border_width = atoi(value);
	}
	else if (strcmp(key, "border-radius") == 0) {
		cfg->tray_menu_widget.border_radius = atoi(value);
	}
}

// cppcheck-suppress unusedFunction
void tray_config_write(FILE *f, const Config *cfg) {
	fprintf(f, "[tray-menu]\n");
	fprintf(f, "background = #%02X%02X%02X\n",
		(int)(cfg->tray_menu.bg.r * 255),
		(int)(cfg->tray_menu.bg.g * 255),
		(int)(cfg->tray_menu.bg.b * 255));
	fprintf(f, "border = #%02X%02X%02X\n",
		(int)(cfg->tray_menu.border.r * 255),
		(int)(cfg->tray_menu.border.g * 255),
		(int)(cfg->tray_menu.border.b * 255));
	fprintf(f, "color = #%02X%02X%02X\n",
		(int)(cfg->tray_menu.fg.r * 255),
		(int)(cfg->tray_menu.fg.g * 255),
		(int)(cfg->tray_menu.fg.b * 255));
	fprintf(f, "font-family = %s\n", cfg->tray_menu.font_family);
	fprintf(f, "font-size = %d\n", cfg->tray_menu.font_size);
	fprintf(f, "hover-background = #%02X%02X%02X\n\n",
		(int)(cfg->tray_menu.hover_bg.r * 255),
		(int)(cfg->tray_menu.hover_bg.g * 255),
		(int)(cfg->tray_menu.hover_bg.b * 255));
}
