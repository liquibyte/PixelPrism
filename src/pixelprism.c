/* pixelprism.c - Main Application Implementation
 *
 * PixelPrism: A lightweight X11 color picker and palette tool
 *
 * Main application implementation coordinating all widgets and handling the event loop.
 * Manages color conversions, widget updates, configuration watching, and user interactions.
 *
 * Features:
 * - Real-time color picking with magnified zoom
 * - Multiple color format displays (RGB, HSV, HSL, Hex)
 * - Live color format conversions
 * - Configuration file watching and hot-reloading
 * - About dialog with application information
 * - System tray integration for minimizing
 * - Clipboard integration for color values
 * - Comprehensive theming system
 *
 * This is the main entry point and event coordinator for the PixelPrism
 * application, integrating all widget subsystems into a cohesive color picker tool.
 */

#include "pixelprism.h"
#include "swatch.h"
#include "button.h"
#include "entry.h"
#include "config.h"
#include "config_registry.h"
#include "menu.h"
#include "label.h"
#include "tray.h"
#include "dbe.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <math.h>
#include <signal.h>
#include <fontconfig/fontconfig.h>
#include <X11/Xatom.h>
#include <X11/xpm.h>
#include <time.h>
#include <sys/time.h>

/* ========== PERSISTED STATE HELPERS ========== */

/*
 * Helpers read and write the user's last-known window position, selected
 * color, and zoom magnification under ~/.config/pixelprism/window.dat.
 * Keeping these functions local allows the main event loop to treat state
 * persistence as simple black-box calls.
 */

static void state_build_path(char *out, size_t size) {
	const char *home = getenv("HOME");
	if (!home) {
		home = ".";
	}
	snprintf(out, size, "%s/.config/pixelprism/window.dat", home);
}

static void state_ensure_dir(void) {
	const char *home = getenv("HOME");
	if (!home) {
		home = ".";
	}
	char dir_path[PATH_MAX];
	snprintf(dir_path, sizeof(dir_path), "%s/.config/pixelprism", home);
	mkdir(dir_path, 0755);
}

int state_load_window_position(int *x, int *y) {
	if (!x || !y) {
		return -1;
	}
	char path[PATH_MAX];
	state_build_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	char line[256];
	int found_x = 0, found_y = 0;
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#' || line[0] == '\n') {
			continue;
		}
		char key[128], value[128];
		if (sscanf(line, "%127[^=]=%127s", key, value) == 2) {
			if (strcmp(key, "window-x") == 0) {
				*x = atoi(value);
				found_x = 1;
			}
			else if (strcmp(key, "window-y") == 0) {
				*y = atoi(value);
				found_y = 1;
			}
		}
	}
	fclose(f);
	return (found_x && found_y) ? 0 : -1;
}

int state_load_zoom_mag(int *zoom_mag_out) {
	if (!zoom_mag_out) {
		return 0;
	}
	char path[PATH_MAX];
	state_build_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) {
		return 0;
	}
	char line[256];
	int found = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "zoom-mag=", 9) == 0) {
			*zoom_mag_out = atoi(line + 9);
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

int state_load_last_color(char hex_out[8]) {
	if (!hex_out) {
		return 0;
	}
	char path[PATH_MAX];
	state_build_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) {
		return 0;
	}
	char line[256];
	int found = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "current-color=", 14) == 0) {
			char *val = line + 14;
			char *end = val + strlen(val) - 1;
			while (end >= val && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
				*end-- = '\0';
			}
			if (val[0] != '#') {
				hex_out[0] = '#';
				strncpy(hex_out + 1, val, 6);
			}
			else {
				strncpy(hex_out, val, 7);
			}
			hex_out[7] = '\0';
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

int state_save_window_position(int x, int y) {
	state_ensure_dir();
	char path[PATH_MAX];
	state_build_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	char existing_color[64] = {0};
	int existing_zoom = 0;
	int has_zoom = state_load_zoom_mag(&existing_zoom);
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "current-color=", 14) == 0) {
				strncpy(existing_color, line + 14, sizeof(existing_color) - 1);
				break;
			}
		}
		fclose(f);
	}
	f = fopen(path, "w");
	if (!f) {
		return -1;
	}
	fprintf(f, "# PixelPrism Application State\n");
	fprintf(f, "# This file is automatically managed - do not edit\n\n");
	fprintf(f, "window-x=%d\n", x);
	fprintf(f, "window-y=%d\n", y);
	if (existing_color[0] != '\0') {
		fprintf(f, "current-color=%s", existing_color);
		if (existing_color[strlen(existing_color) - 1] != '\n') {
			fputc('\n', f);
		}
	}
	if (has_zoom) {
		fprintf(f, "zoom-mag=%d\n", existing_zoom);
	}
	fclose(f);
	return 0;
}

int state_save_zoom_mag(int zoom_mag) {
	state_ensure_dir();
	char path[PATH_MAX];
	state_build_path(path, sizeof(path));
	int x = 0, y = 0;
	state_load_window_position(&x, &y);
	char color_hex[8];
	int has_color = state_load_last_color(color_hex);
	FILE *f = fopen(path, "w");
	if (!f) {
		return -1;
	}
	fprintf(f, "# PixelPrism Application State\n");
	fprintf(f, "# This file is automatically managed - do not edit\n\n");
	fprintf(f, "window-x=%d\n", x);
	fprintf(f, "window-y=%d\n", y);
	if (has_color) {
		fprintf(f, "current-color=%s\n", color_hex);
	}
	fprintf(f, "zoom-mag=%d\n", zoom_mag);
	fclose(f);
	return 0;
}

int state_save_last_color(const char *hex) {
	if (!hex) {
		return -1;
	}
	state_ensure_dir();
	char path[PATH_MAX];
	state_build_path(path, sizeof(path));
	int x = 0, y = 0;
	state_load_window_position(&x, &y);
	int zoom_mag = 0;
	int has_zoom = state_load_zoom_mag(&zoom_mag);
	FILE *f = fopen(path, "w");
	if (!f) {
		return -1;
	}
	fprintf(f, "# PixelPrism Application State\n");
	fprintf(f, "# This file is automatically managed - do not edit\n\n");
	fprintf(f, "window-x=%d\n", x);
	fprintf(f, "window-y=%d\n", y);
	fprintf(f, "current-color=%s\n", hex);
	if (has_zoom) {
		fprintf(f, "zoom-mag=%d\n", zoom_mag);
	}
	fclose(f);
	return 0;
}

/* Global X resources */
Display *display;
Screen *screen;
Window root_window, main_window, zoom_window;
XSetWindowAttributes xsmwa;
GC zoom_gc;
ClipboardContext *clipboard_ctx = NULL;


int button_press = False;
volatile sig_atomic_t running = 1;

/* Color text buffers - now using local buffers for efficiency */

/* String constants for optimization */
static const char *FORMAT_HSV_HSL = "%.1lf° %.1lf%% %.1lf%%";
static const char *FORMAT_RGBF = "%.3f, %.3f, %.3f";
static const char *FORMAT_RGBI = "%d, %d, %d";

/**
 * format_hex - Format RGB8 to hex string with case preference
 * @rgb8: RGB color in 8-bit format
 * @out_hex Output buffer (must be at least 8 bytes)
 * @uppercase 0 for lowercase (#ff00aa), 1 for uppercase (#FF00AA)
 *
 * UI layer helper that applies hex case preference to color output.
 */
static void format_hex(RGB8 rgb8, char *out_hex, int uppercase) {
	rgb8_to_hex(rgb8, out_hex); // Always returns uppercase
	if (!uppercase) {
		// Convert to lowercase
		for (int i = 0; out_hex[i]; i++) {
			if (out_hex[i] >= 'A' && out_hex[i] <= 'F') {
				out_hex[i] = (char)(out_hex[i] + 32); // A-F -> a-f
			}
		}
	}
}

/* ========== GLOBAL WIDGET CONTEXTS ========== */

/* Persistent widget contexts */
MiniEntry *entry_hsv = NULL;
MiniEntry *entry_hsl = NULL;
MiniEntry *entry_rgbf = NULL;
MiniEntry *entry_rgbi = NULL;
MiniEntry *entry_hex = NULL;

/* Label widgets for entries */
static LabelContext *label_hsv = NULL;
static LabelContext *label_hsl = NULL;
static LabelContext *label_rgbf = NULL;
static LabelContext *label_rgbi = NULL;
static LabelContext *label_hex = NULL;

SwatchContext *swatch_ctx = NULL;
static ButtonContext *button_ctx = NULL;
static MenuBar *menubar = NULL;

/* Widget context pointers - maintain independence between widgets */
static AboutWindow *about_win = NULL; /* About dialog window context */
static ZoomContext *zoom_ctx = NULL; /* Zoom/magnifier context */
static TrayContext *tray_ctx = NULL; /* System tray icon context */

/* Icon XPM data (defined in icons.c) */
extern char *pixelprism_xpm[];
extern char *pixelprism_icon_xpm[];

/* ========== CONFIGURATION & STATE DATA ========== */

/* Configuration and theme management */
static MiniTheme current_theme = {0}; /* Current application theme */
static int inotify_fd = -1; /* File descriptor for config watching */
static int watch_fd = -1; /* Watch descriptor for config directory */

/* State management to prevent infinite loops */
static int updating_from_callback = 0; /* Flag to prevent callback cascades */

/* Current color state - tracks the application's active color */
static RGB8 current_rgb8 = {
	0, 0, 0
};
static RGBf current_rgbf = {
	0.0, 0.0, 0.0
};
/* Loaded color state - retained for internal helper usage */
static RGBf loaded_rgbf = {
	0.0, 0.0, 0.0
};

/* ========== HELPER FUNCTIONS ========== */

/* --- Color Comparison --- */
static int rgb8_equal(RGB8 a, RGB8 b) {
	return (a.r == b.r) && (a.g == b.g) && (a.b == b.b);
}

/* Forward declarations */
static void format_and_update_entries_from_rgbf(RGBf rgbf);
static void reload_theme(void);
static void refresh_entry_from_current(const MiniEntry *e);

/* --- Zoom Activation Callback --- */
static void on_zoom_activated(ZoomContext *zoom, void *user_data) {
	(void)zoom; // Unused
	ButtonContext *btn = (ButtonContext *)user_data;
	button_set_pressed(btn, 1);
}

/* --- Validation Helpers --- */
static long long get_time_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

static void validate_entry_flash_valid(MiniEntry *e) {
	if (!e) {
		return;
	}
	// Clear any previous invalid state
	if (entry_get_validation_state(e) == 1) {
		entry_set_validation_state(e, 0);
	}
	// Start green flash
	entry_set_validation_state(e, 2);
	entry_set_validation_flash_start(e, get_time_ms());
}

static void validate_entry_flash_invalid_and_restore(MiniEntry *e) {
	if (!e) {
		return;
	}
	// Start red flash
	entry_set_validation_state(e, 1);
	entry_set_validation_flash_start(e, get_time_ms());
	
	// Restore text immediately
	refresh_entry_from_current(e);
}

static void update_validation_timers(void) {
	MiniEntry *entries[] = {entry_hsv, entry_hsl, entry_rgbf, entry_rgbi, entry_hex};
	long long now = get_time_ms();
	
	for (int i = 0; i < 5; i++) {
		MiniEntry *e = entries[i];
		if (!e) {
			continue;
		}
		
		int state = entry_get_validation_state(e);
		long long flash_start = entry_get_validation_flash_start(e);
		
		// Clear red flash after 150ms
		if (state == 1 && flash_start > 0) {
			if (now - flash_start >= 150) {
				entry_set_validation_state(e, 0);
			}
		}
		// Clear green flash after 500ms
		else if (state == 2 && flash_start > 0) {
			if (now - flash_start >= 500) {
				entry_set_validation_state(e, 0);
			}
		}
	}
}

/* --- Entry Widget Refresh --- */
/**
 * refresh_entry_from_current - Update a single color entry from current color state
 * @e Pointer to the entry widget to update
 *
 * This function updates only the specified entry widget with the current
 * color in the appropriate format (HSV, HSL, RGB float, RGB int, or hex).
 * Used for validation restoration when invalid input is detected.
 *
 * @param e Entry to refresh
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
static void refresh_entry_from_current(const MiniEntry *e) {
	if (!e) {
		return;
	}
	char buf[256];
	// Update HSV entry
	if (e == entry_hsv) {
		HSV hsv = rgb_to_hsv(current_rgbf);
		snprintf(buf, sizeof(buf), FORMAT_HSV_HSL, hsv.H, hsv.S * 100.0, hsv.V * 100.0);
		buf[sizeof(buf) - 1] = '\0';
		entry_set_text(entry_hsv, buf);
		return;
	}
	// Update HSL entry
	if (e == entry_hsl) {
		HSL hsl = rgb_to_hsl(current_rgbf);
		snprintf(buf, sizeof(buf), FORMAT_HSV_HSL, hsl.H, hsl.S * 100.0, hsl.L * 100.0);
		buf[sizeof(buf) - 1] = '\0';
		entry_set_text(entry_hsl, buf);
		return;
	}
	// Update RGB float entry
	if (e == entry_rgbf) {
		snprintf(buf, sizeof(buf), FORMAT_RGBF, current_rgbf.r, current_rgbf.g, current_rgbf.b);
		buf[sizeof(buf) - 1] = '\0';
		entry_set_text(entry_rgbf, buf);
		return;
	}
	// Update RGB integer entry
	if (e == entry_rgbi) {
		RGB8 rgb8 = rgbf_to_rgb8(current_rgbf);
		snprintf(buf, sizeof(buf), FORMAT_RGBI, rgb8.r, rgb8.g, rgb8.b);
		buf[sizeof(buf) - 1] = '\0';
		entry_set_text(entry_rgbi, buf);
		return;
	}
	// Update hex entry
	if (e == entry_hex) {
		format_hex(current_rgb8, buf, current_theme.hex_uppercase);
		entry_set_text(entry_hex, buf);
		return;
	}
}

#pragma GCC diagnostic pop

/* --- X11 Color Utilities --- */
/* Color conversion and font loading now centralized in config.c */

struct AboutWindow {
	Display *dpy;
	Window parent;
	Window win;
	int screen;
	int visible;
	XftDraw *draw;
	XftFont *font;
	XftColor xft_fg;
	XftColor xft_link;
	unsigned long bg_pixel;
	int link_hover;
	int link_underline;
	Cursor hand_cursor;
	Cursor normal_cursor;
	int parent_x, parent_y;
	char browser_path[256];
	Pixmap icon_pixmap;
	Pixmap icon_mask;
	int icon_width;
	int icon_height;
	int width;
	int height;
};

/* About window will use main window dimensions */

static void about_draw(struct AboutWindow *win);

AboutWindow *about_create(Display *dpy, Window parent, const MiniTheme *theme) {
	AboutWindow *win = calloc(1, sizeof(AboutWindow));
	if (!win) {
		return NULL;
	}
	win->dpy = dpy;
	win->parent = parent;
	win->screen = DefaultScreen(dpy);
	win->visible = 0;
	win->link_hover = 0;
	win->parent_x = 0;
	win->parent_y = 0;
	win->width = theme->main.about_width;
	win->height = theme->main.about_height;

	// Background and typography from main
	win->bg_pixel = config_color_to_pixel(dpy, win->screen, theme->main.background);
	win->font = config_open_font(dpy, win->screen, theme->main.font_family, theme->main.font_size);

	// Text/link colors from main
	ConfigColor text_c = theme->main.text_color;
	XRenderColor xr_text;
	#define CLAMP_COMP(c) ((c) < 0.0 ? 0 : (c) > 1.0 ? 65535 : (unsigned short)((c) * 65535.0 + 0.5))
	xr_text.red = CLAMP_COMP(text_c.r);
	xr_text.green = CLAMP_COMP(text_c.g);
	xr_text.blue = CLAMP_COMP(text_c.b);
	xr_text.alpha = CLAMP_COMP(text_c.a);
	XftColorAllocValue(dpy, DefaultVisual(dpy, win->screen), DefaultColormap(dpy, win->screen), &xr_text, &win->xft_fg);

	ConfigColor link_c = theme->main.link_color;
	XRenderColor xr_link;
	xr_link.red = CLAMP_COMP(link_c.r);
	xr_link.green = CLAMP_COMP(link_c.g);
	xr_link.blue = CLAMP_COMP(link_c.b);
	xr_link.alpha = CLAMP_COMP(link_c.a);
	#undef CLAMP_COMP
	XftColorAllocValue(dpy, DefaultVisual(dpy, win->screen), DefaultColormap(dpy, win->screen), &xr_link, &win->xft_link);

	// Link underline setting
	win->link_underline = theme->main.link_underline;

	// Create cursors
	win->normal_cursor = XCreateFontCursor(dpy, XC_left_ptr);
	win->hand_cursor = XCreateFontCursor(dpy, XC_hand2);

	strncpy(win->browser_path, theme->browser_path, sizeof(win->browser_path) - 1);
	win->browser_path[sizeof(win->browser_path) - 1] = '\0';

	// Load icon from XPM data
	win->icon_pixmap = None;
	win->icon_mask = None;
	win->icon_width = 0;
	win->icon_height = 0;

	XpmAttributes xpm_attrs;
	xpm_attrs.valuemask = XpmReturnPixels | XpmReturnExtensions;
	int xpm_result = XpmCreatePixmapFromData(dpy, parent, pixelprism_xpm, &win->icon_pixmap, &win->icon_mask, &xpm_attrs);
	if (xpm_result == XpmSuccess) {
		win->icon_width = (int)xpm_attrs.width;
		win->icon_height = (int)xpm_attrs.height;
		XpmFreeAttributes(&xpm_attrs);
	}
	return win;
}

void about_destroy(AboutWindow *win) {
	if (!win) {
		return;
	}
	if (win->visible) {
		// ensure hidden and resources freed
		XUngrabPointer(win->dpy, CurrentTime);
		if (win->draw) {
			XftDrawDestroy(win->draw);
			win->draw = NULL;
		}
		if (win->win) {
			XDestroyWindow(win->dpy, win->win);
			win->win = 0;
		}
		win->visible = 0;
	}
	if (win->font) {
		XftFontClose(win->dpy, win->font);
	}
	XftColorFree(win->dpy, DefaultVisual(win->dpy, win->screen), DefaultColormap(win->dpy, win->screen), &win->xft_fg);
	XftColorFree(win->dpy, DefaultVisual(win->dpy, win->screen), DefaultColormap(win->dpy, win->screen), &win->xft_link);
	if (win->normal_cursor) {
		XFreeCursor(win->dpy, win->normal_cursor);
	}
	if (win->hand_cursor) {
		XFreeCursor(win->dpy, win->hand_cursor);
	}
	// Free icon pixmaps
	if (win->icon_pixmap != None) {
		XFreePixmap(win->dpy, win->icon_pixmap);
	}
	if (win->icon_mask != None) {
		XFreePixmap(win->dpy, win->icon_mask);
	}
	free(win);
}

void about_set_theme(AboutWindow *win, const MiniTheme *theme) {
	if (!win || !theme) {
		return;
	}
	// Update about window size from theme
	win->width = theme->main.about_width;
	win->height = theme->main.about_height;
	win->bg_pixel = config_color_to_pixel(win->dpy, win->screen, theme->main.background);
	if (win->visible && win->win) {
		XSetWindowBackground(win->dpy, win->win, win->bg_pixel);
	}
	if (win->font) {
		XftFontClose(win->dpy, win->font);
		win->font = NULL;
	}
	win->font = config_open_font(win->dpy, win->screen, theme->main.font_family, theme->main.font_size);
	XftColorFree(win->dpy, DefaultVisual(win->dpy, win->screen), DefaultColormap(win->dpy, win->screen), &win->xft_fg);
	XftColorFree(win->dpy, DefaultVisual(win->dpy, win->screen), DefaultColormap(win->dpy, win->screen), &win->xft_link);
	ConfigColor text_c2 = theme->main.text_color;
	XRenderColor xr_text;
	#define CLAMP_COMP(c) ((c) < 0.0 ? 0 : (c) > 1.0 ? 65535 : (unsigned short)((c) * 65535.0 + 0.5))
	xr_text.red = CLAMP_COMP(text_c2.r);
	xr_text.green = CLAMP_COMP(text_c2.g);
	xr_text.blue = CLAMP_COMP(text_c2.b);
	xr_text.alpha = CLAMP_COMP(text_c2.a);
	XftColorAllocValue(win->dpy, DefaultVisual(win->dpy, win->screen), DefaultColormap(win->dpy, win->screen), &xr_text, &win->xft_fg);
	ConfigColor link_c2 = theme->main.link_color;
	XRenderColor xr_link;
	xr_link.red = CLAMP_COMP(link_c2.r);
	xr_link.green = CLAMP_COMP(link_c2.g);
	xr_link.blue = CLAMP_COMP(link_c2.b);
	xr_link.alpha = CLAMP_COMP(link_c2.a);
	#undef CLAMP_COMP
	XftColorAllocValue(win->dpy, DefaultVisual(win->dpy, win->screen), DefaultColormap(win->dpy, win->screen), &xr_link, &win->xft_link);
	win->link_underline = theme->main.link_underline;
	strncpy(win->browser_path, theme->browser_path, sizeof(win->browser_path) - 1);
	win->browser_path[sizeof(win->browser_path) - 1] = '\0';
	if (win->visible && win->win) {
		XEvent ev = {
			0
		};
		ev.type = Expose;
		ev.xexpose.window = win->win;
		ev.xexpose.count = 0;
		XSendEvent(win->dpy, win->win, False, ExposureMask, &ev);
		XFlush(win->dpy);
	}
}

void about_show(AboutWindow *win) {
	if (!win || win->visible) {
		return;
	}
	XWindowAttributes parent_attr;
	XGetWindowAttributes(win->dpy, win->parent, &parent_attr);
	Window child;
	XTranslateCoordinates(win->dpy, win->parent, DefaultRootWindow(win->dpy), 0, 0, &win->parent_x, &win->parent_y, &child);
	int x = win->parent_x + (parent_attr.width - win->width) / 2;
	int y = win->parent_y + (parent_attr.height - win->height) / 2;
	XSetWindowAttributes attr;
	attr.override_redirect = True;
	attr.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask;
	attr.background_pixel = win->bg_pixel;
	win->win = XCreateWindow(win->dpy, DefaultRootWindow(win->dpy), x, y, (unsigned int)win->width, (unsigned int)win->height, 0, DefaultDepth(win->dpy, win->screen), InputOutput, DefaultVisual(win->dpy, win->screen), CWOverrideRedirect | CWEventMask | CWBackPixel, &attr);
	win->draw = XftDrawCreate(win->dpy, win->win, DefaultVisual(win->dpy, win->screen), DefaultColormap(win->dpy, win->screen));
	XMapRaised(win->dpy, win->win);
	win->visible = 1;
	XGrabPointer(win->dpy, win->win, False, ButtonPressMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
}

void about_hide(AboutWindow *win) {
	if (!win || !win->visible) {
		return;
	}
	XUngrabPointer(win->dpy, CurrentTime);
	if (win->draw) {
		XftDrawDestroy(win->draw);
		win->draw = NULL;
	}
	if (win->win) {
		XDestroyWindow(win->dpy, win->win);
		win->win = 0;
	}
	win->visible = 0;
	win->link_hover = 0;
}

int about_is_visible(AboutWindow *win) {
	return win ? win->visible : 0;
}

static void about_draw(AboutWindow *win) {
	if (!win || !win->visible) {
		return;
	}
	XClearWindow(win->dpy, win->win);

	// Draw icon at the top center
	int icon_y = 20;
	if (win->icon_pixmap != None) {
		int icon_x = (win->width - win->icon_width) / 2;

		GC gc = XCreateGC(win->dpy, win->win, 0, NULL);
		if (win->icon_mask != None) {
			XSetClipMask(win->dpy, gc, win->icon_mask);
			XSetClipOrigin(win->dpy, gc, icon_x, icon_y);
		}
		XCopyArea(win->dpy, win->icon_pixmap, win->win, gc, 0, 0, (unsigned int)win->icon_width, (unsigned int)win->icon_height, icon_x, icon_y);
		XFreeGC(win->dpy, gc);

		icon_y += win->icon_height + 15; // Space below icon
	}
	const char *lines[] = {
		"PixelPrism is a small color picker application that allows you to view colors in",
		"multiple formats: HSV, HSL, RGB 0-1, 0-255 and Hexidecimal.  The configuration",
		"file allows customization of all the widgets in the program including colors,",
		"fonts, layouts, sizes and even the path for your prefered text editor to edit",
		"the configuration.",
		"Click to close.",
		"",
		"https://github.com/liquibyte/PixelPrism"
	};
	int line_height = win->font->ascent + win->font->descent + 4;
	int start_y = icon_y;
	for (int i = 0; i < 8; i++) {
		int y = start_y + i * line_height + win->font->ascent;
		XGlyphInfo extents;
		XftTextExtentsUtf8(win->dpy, win->font, (const FcChar8 *)lines[i], (int)strlen(lines[i]), &extents);
		int x = (win->width - extents.xOff) / 2;
		if (i == 7) {
			XftDrawStringUtf8(win->draw, &win->xft_link, win->font, x, y, (const FcChar8 *)lines[i], (int)strlen(lines[i]));
			// Draw underline if enabled OR on hover
			if (win->link_underline || win->link_hover) {
				GC gc = XCreateGC(win->dpy, win->win, 0, NULL);
				XColor xc = {0};
				xc.red = win->xft_link.color.red;
				xc.green = win->xft_link.color.green;
				xc.blue = win->xft_link.color.blue;
				xc.flags = DoRed | DoGreen | DoBlue;
				Colormap cmap = DefaultColormap(win->dpy, win->screen);
				XAllocColor(win->dpy, cmap, &xc);
				XSetForeground(win->dpy, gc, xc.pixel);
				XDrawLine(win->dpy, win->win, gc, x, y + 2, x + extents.xOff, y + 2);
				XFreeGC(win->dpy, gc);
			}
		}
		else {
			XftDrawStringUtf8(win->draw, &win->xft_fg, win->font, x, y, (const FcChar8 *)lines[i], (int)strlen(lines[i]));
		}
	}
}

int about_handle_event(AboutWindow *win, XEvent *ev) {
	if (!win || !win->visible) {
		return 0;
	}
	if (ev->type == ConfigureNotify && ev->xany.window == win->parent) {
		Window child;
		int new_parent_x, new_parent_y;
		XTranslateCoordinates(win->dpy, win->parent, DefaultRootWindow(win->dpy), 0, 0, &new_parent_x, &new_parent_y, &child);
		if (new_parent_x != win->parent_x || new_parent_y != win->parent_y) {
			int dx = new_parent_x - win->parent_x;
			int dy = new_parent_y - win->parent_y;
			win->parent_x = new_parent_x;
			win->parent_y = new_parent_y;
			XWindowAttributes attr;
			XGetWindowAttributes(win->dpy, win->win, &attr);
			XMoveWindow(win->dpy, win->win, attr.x + dx, attr.y + dy);
		}
		return 0;
	}
	if (ev->xany.window != win->win) {
		return 0;
	}
	switch (ev->type) {
		case Expose:
			about_draw(win);
			return 1;
		case ButtonPress:
			if (ev->xbutton.button == Button1) {
				int line_height = win->font->ascent + win->font->descent + 4;
				// Calculate start_y same as in about_draw
				int icon_y = 20;
				if (win->icon_pixmap != None) {
					icon_y += win->icon_height + 15;
				}
				int start_y = icon_y;
				int link_y = start_y + 7 * line_height;
				if (ev->xbutton.y >= link_y && ev->xbutton.y <= link_y + line_height) {
					pid_t pid = fork();
					if (pid == 0) {
						execlp(win->browser_path, win->browser_path, "https://github.com/liquibyte/PixelPrism", NULL);
						execlp("xdg-open", "xdg-open", "https://github.com/liquibyte/PixelPrism", NULL);
						exit(1);
					}
				}
				about_hide(win);
				return 1;
			}
			if (ev->xbutton.button == Button3) {
				about_hide(win);
				return 1;
			}
		break;
		case MotionNotify: {
			int line_height = win->font->ascent + win->font->descent + 4;
			// Calculate start_y same as in about_draw
			int icon_y = 20;
			if (win->icon_pixmap != None) {
				icon_y += win->icon_height + 15;
			}
			int start_y = icon_y;
			int link_y = start_y + 7 * line_height;
			int hover = (ev->xmotion.y >= link_y && ev->xmotion.y <= link_y + line_height);
			if (hover != win->link_hover) {
				win->link_hover = hover;
				XDefineCursor(win->dpy, win->win, hover ? win->hand_cursor : win->normal_cursor);
				about_draw(win);
			}
			return 1;
		}
	}
	return 0;
}

/* --- Color State Management --- */
static int rgbf_equal_eps(RGBf a, RGBf b, double eps) {
	double dr = fabs(a.r - b.r);
	double dg = fabs(a.g - b.g);
	double db = fabs(a.b - b.b);
	return (dr <= eps) && (dg <= eps) && (db <= eps);
}

/* Helper functions for cursor management */
/* Initialize color state - displays restored color from state or config default */
static void initialize_color_state(void) {
	// Try to load last color from state (window.dat)
	char hex[8];
	RGB8 rgb8;
	int has_state_color = state_load_last_color(hex) && hex_to_rgb8(hex, &rgb8);
	RGBf src;
	if (has_state_color) {
		src = rgb8_to_rgbf(rgb8);
	}
	else {
		// Fallback to color from config (or black if first run)
		src = (RGBf){
			current_theme.current_color.r,
			current_theme.current_color.g,
			current_theme.current_color.b
		};
	}
	current_rgbf = src;
	current_rgb8 = rgbf_to_rgb8(src);
	loaded_rgbf = src;

	// Force entries to update even if the value matches
	updating_from_callback = 1;
	RGBf saved = current_rgbf;
	current_rgbf = (RGBf){
		-1.0, -1.0, -1.0
	};
	format_and_update_entries_from_rgbf(saved);

	// Load saved zoom image
	const char *home = getenv("HOME");
	if (!home) {
		home = ".";
	}
	char zoom_path[512];
	snprintf(zoom_path, sizeof(zoom_path), "%s/.config/pixelprism/last_zoom.dat", home);
	zoom_load_image(zoom_ctx, zoom_path);

	updating_from_callback = 0;
}

/* DO NOT REMOVE THIS FUNCTION, IT MAKES THE RESET MENU ITEM WORK */
/* Reset all widgets to black */
static void reset_to_black(void) {
	XColor color = {0};
	Colormap cmap = DefaultColormap(display, DefaultScreen(display));
	color.red = color.green = color.blue = 0;
	color.flags = DoRed | DoGreen | DoBlue;
	XAllocColor(display, cmap, &color);

	updating_from_callback = 1;
	swatch_set_color(swatch_ctx, color.pixel);
	entry_set_text(entry_hsv, "0° 0% 0%");
	entry_set_text(entry_hsl, "0° 0% 0%");
	char rgbf_buf[50];
	snprintf(rgbf_buf, sizeof(rgbf_buf), "%.3f, %.3f, %.3f", 0.0f, 0.0f, 0.0f);
	entry_set_text(entry_rgbf, rgbf_buf);
	entry_set_text(entry_rgbi, "0, 0, 0");
	entry_set_text(entry_hex, "#000000");
	updating_from_callback = 0;
	XFlush(display);
	current_rgb8 = (RGB8) {
		0, 0, 0
	};
	current_rgbf = (RGBf) {
		0.0, 0.0, 0.0
	};
	// Clear zoom image and delete saved file
	zoom_clear_image(zoom_ctx);
	// Persist black as the last color so initialize_color_state() restores it
	state_save_last_color("#000000");
	const char *home = getenv("HOME");
	if (!home) {
		home = ".";
	}
	char zoom_path[512];
	snprintf(zoom_path, sizeof(zoom_path), "%s/.config/pixelprism/last_zoom.dat", home);
	unlink(zoom_path); // Delete the saved zoom image
}

/* --- Configuration Management --- */
/* Open configuration file in editor */
static void open_configuration(void) {
	const char *home = getenv("HOME");
	if (!home) {
		home = ".";
	}
	char config_path[PATH_MAX];
	snprintf(config_path, sizeof(config_path), "%s/.config/pixelprism/pixelprism.conf", home);
	// Check if config file exists, create if not
	if (access(config_path, F_OK) != 0) {
		fprintf(stderr, "Config file not found, creating default: %s\n", config_path);
		// Ensure config directory exists
		char config_dir[PATH_MAX];
		snprintf(config_dir, sizeof(config_dir), "%s/.config/pixelprism", home);
		mkdir(config_dir, 0755); // Create directory (ignore error if exists)
		// Write default config with current theme values
		if (config_write_defaults(config_path) != 0) {
			fprintf(stderr, "Warning: Failed to create default config file\n");
		}
		else {
			fprintf(stderr, "Default config file created successfully\n");
		}
	}
	pid_t pid = fork();
	if (pid == 0) {
		// Child process
		execlp(current_theme.editor_path, current_theme.editor_path, config_path, NULL);
		// If configured editor fails, try nano as fallback
		execlp("/usr/bin/nano", "/usr/bin/nano", config_path, NULL);
		// If nano fails, try xdg-open as fallback
		execlp("xdg-open", "xdg-open", config_path, NULL);
		exit(1);
	}
	else if (pid > 0) {
		// Parent process - don't wait for editor
		// The editor will run asynchronously
	}
}

/* --- Color Format Parsers --- */
/* Parse and validate each format */
static int parse_hsv(const char *text, double *h, double *s, double *v) {
	char buf[256];
	strncpy(buf, text, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	for (int i = 0; buf[i]; i++) {
		if (buf[i] == '\xc2' || buf[i] == '\xb0' || buf[i] == '%' || buf[i] == ',') {
			buf[i] = ' ';
		}
	}
	int count = sscanf(buf, "%lf %lf %lf", h, s, v);
	if (count != 3) {
		return 0;
	}
	if (*h < 0 || *h > 360) {
		return 0;
	}
	if (*s < 0 || *s > 100) {
		return 0;
	}
	if (*v < 0 || *v > 100) {
		return 0;
	}
	*s /= 100.0;
	*v /= 100.0;
	return 1;
}

static int parse_hsl(const char *text, double *h, double *s, double *l) {
	char buf[256];
	strncpy(buf, text, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	for (int i = 0; buf[i]; i++) {
		if (buf[i] == '\xc2' || buf[i] == '\xb0' || buf[i] == '%' || buf[i] == ',') {
			buf[i] = ' ';
		}
	}
	int count = sscanf(buf, "%lf %lf %lf", h, s, l);
	if (count != 3) {
		return 0;
	}
	if (*h < 0 || *h > 360) {
		return 0;
	}
	if (*s < 0 || *s > 100) {
		return 0;
	}
	if (*l < 0 || *l > 100) {
		return 0;
	}
	*s /= 100.0;
	*l /= 100.0;
	return 1;
}

static int parse_rgbf(const char *text, double *r, double *g, double *b) {
	char buf[256];
	strncpy(buf, text, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	for (int i = 0; buf[i]; i++) {
		if (buf[i] == ',') {
			buf[i] = ' ';
		}
	}
	int count = sscanf(buf, "%lf %lf %lf", r, g, b);
	if (count != 3) {
		return 0;
	}
	if (*r < 0 || *r > 1) {
		return 0;
	}
	if (*g < 0 || *g > 1) {
		return 0;
	}
	if (*b < 0 || *b > 1) {
		return 0;
	}
	return 1;
}

static int parse_rgbi(const char *text, int *r, int *g, int *b) {
	char buf[256];
	strncpy(buf, text, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	for (int i = 0; buf[i]; i++) {
		if (buf[i] == ',') {
			buf[i] = ' ';
		}
	}
	int count = sscanf(buf, "%d %d %d", r, g, b);
	if (count != 3) {
		return 0;
	}
	if (*r < 0 || *r > 255) {
		return 0;
	}
	if (*g < 0 || *g > 255) {
		return 0;
	}
	if (*b < 0 || *b > 255) {
		return 0;
	}
	return 1;
}

static int parse_hex(const char *text, RGB8 *out) {
	char buf[256];
	strncpy(buf, text, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;

	int j = 0;
	for (int i = 0; buf[i]; i++) {
		if (buf[i] != ' ' && buf[i] != ',') {
			buf[j++] = buf[i];
		}
	}
	buf[j] = 0;

	return hex_to_rgb8(buf, out);
}

/* --- Color Conversion & Auto-Copy --- */
/**
 * auto_copy_color - Auto-copy color to clipboard in configured format
 * @rgb8: RGB color in 8-bit format
 * @rgbf RGB color in float format
 * @hsv HSV color
 * @hsl HSL color
 *
 * Copies the picked color to clipboard if auto-copy is enabled.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
static void auto_copy_color(RGB8 rgb8, RGBf rgbf, HSV hsv, HSL hsl) {
	if (!current_theme.auto_copy || !clipboard_ctx) {
		return;
	}
	char buf[256];
	const char *format = current_theme.auto_copy_format;
	if (strcmp(format, "hex") == 0) {
		format_hex(rgb8, buf, current_theme.hex_uppercase);
		// Remove # prefix if disabled
		if (!current_theme.hex_prefix && buf[0] == '#') {
			clipboard_set_text(clipboard_ctx, main_window, buf + 1, SELECTION_CLIPBOARD);
		}
		else {
			clipboard_set_text(clipboard_ctx, main_window, buf, SELECTION_CLIPBOARD);
		}
	}
	else if (strcmp(format, "hsv") == 0) {
		snprintf(buf, sizeof(buf), FORMAT_HSV_HSL, hsv.H, hsv.S * 100.0, hsv.V * 100.0);
		clipboard_set_text(clipboard_ctx, main_window, buf, SELECTION_CLIPBOARD);
	}
	else if (strcmp(format, "hsl") == 0) {
		snprintf(buf, sizeof(buf), FORMAT_HSV_HSL, hsl.H, hsl.S * 100.0, hsl.L * 100.0);
		clipboard_set_text(clipboard_ctx, main_window, buf, SELECTION_CLIPBOARD);
	}
	else if (strcmp(format, "rgb") == 0) {
		snprintf(buf, sizeof(buf), FORMAT_RGBF, rgbf.r, rgbf.g, rgbf.b);
		clipboard_set_text(clipboard_ctx, main_window, buf, SELECTION_CLIPBOARD);
	}
	else if (strcmp(format, "rgbi") == 0) {
		snprintf(buf, sizeof(buf), FORMAT_RGBI, rgb8.r, rgb8.g, rgb8.b);
		clipboard_set_text(clipboard_ctx, main_window, buf, SELECTION_CLIPBOARD);
	}
}

#pragma GCC diagnostic pop

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
static void format_and_update_entries(RGB8 rgb8) {
	// If color unchanged, avoid rewriting entries (prevents rounding churn)
	if (rgb8_equal(rgb8, current_rgb8)) {
		return;
	}
	current_rgb8 = rgb8;
	RGBf rgbf = rgb8_to_rgbf(rgb8);
	current_rgbf = rgbf;
	HSV hsv = rgb_to_hsv(rgbf);
	HSL hsl = rgb_to_hsl(rgbf);

	char buf[256];

	// Batch update all entries without drawing (preserves validation state)
	snprintf(buf, sizeof(buf), FORMAT_HSV_HSL, hsv.H, hsv.S * 100.0, hsv.V * 100.0);
	buf[sizeof(buf) - 1] = '\0';
	entry_set_text_no_draw(entry_hsv, buf);

	snprintf(buf, sizeof(buf), FORMAT_HSV_HSL, hsl.H, hsl.S * 100.0, hsl.L * 100.0);
	buf[sizeof(buf) - 1] = '\0';
	entry_set_text_no_draw(entry_hsl, buf);

	snprintf(buf, sizeof(buf), FORMAT_RGBF, rgbf.r, rgbf.g, rgbf.b);
	buf[sizeof(buf) - 1] = '\0';
	entry_set_text_no_draw(entry_rgbf, buf);

	snprintf(buf, sizeof(buf), FORMAT_RGBI, rgb8.r, rgb8.g, rgb8.b);
	buf[sizeof(buf) - 1] = '\0';
	entry_set_text_no_draw(entry_rgbi, buf);

	format_hex(rgb8, buf, current_theme.hex_uppercase);
	entry_set_text_no_draw(entry_hex, buf);
	
	// Draw all entries once
	entry_draw(entry_hsv);
	entry_draw(entry_hsl);
	entry_draw(entry_rgbf);
	entry_draw(entry_rgbi);
	entry_draw(entry_hex);

	// Auto-copy color to clipboard if enabled
	auto_copy_color(rgb8, rgbf, hsv, hsl);

	XColor color = {0};
	Colormap cmap = DefaultColormap(display, DefaultScreen(display));
	color.red = (unsigned short)(rgb8.r * 257);
	color.green = (unsigned short)(rgb8.g * 257);
	color.blue = (unsigned short)(rgb8.b * 257);
	color.flags = DoRed | DoGreen | DoBlue;
	XAllocColor(display, cmap, &color);
	swatch_set_color(swatch_ctx, color.pixel);
}

#pragma GCC diagnostic pop

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
static void format_and_update_entries_from_rgbf(RGBf rgbf) {
	// If color unchanged, avoid rewriting entries (prevents rounding churn)
	if (rgbf_equal_eps(rgbf, current_rgbf, 1e-6)) {
		return;
	}
	current_rgbf = rgbf;
	// Mark config as changed when color is updated (but not during initialization)
	if (!updating_from_callback) {
		config_mark_changed(&current_theme);
	}
	RGB8 rgb8 = rgbf_to_rgb8(rgbf);
	current_rgb8 = rgb8;
	HSV hsv = rgb_to_hsv(rgbf);
	HSL hsl = rgb_to_hsl(rgbf);

	char buf[256];

	// Batch update all entries without drawing (preserves validation state)
	snprintf(buf, sizeof(buf), FORMAT_HSV_HSL, hsv.H, hsv.S * 100.0, hsv.V * 100.0);
	entry_set_text_no_draw(entry_hsv, buf);

	snprintf(buf, sizeof(buf), FORMAT_HSV_HSL, hsl.H, hsl.S * 100.0, hsl.L * 100.0);
	entry_set_text_no_draw(entry_hsl, buf);

	snprintf(buf, sizeof(buf), FORMAT_RGBF, rgbf.r, rgbf.g, rgbf.b);
	entry_set_text_no_draw(entry_rgbf, buf);

	snprintf(buf, sizeof(buf), FORMAT_RGBI, rgb8.r, rgb8.g, rgb8.b);
	entry_set_text_no_draw(entry_rgbi, buf);

	format_hex(rgb8, buf, current_theme.hex_uppercase);
	entry_set_text_no_draw(entry_hex, buf);
	
	// Draw all entries once
	entry_draw(entry_hsv);
	entry_draw(entry_hsl);
	entry_draw(entry_rgbf);
	entry_draw(entry_rgbi);
	entry_draw(entry_hex);

	// Auto-copy color to clipboard if enabled
	auto_copy_color(rgb8, rgbf, hsv, hsl);

	XColor color = {0};
	Colormap cmap = DefaultColormap(display, DefaultScreen(display));
	color.red = (unsigned short)(rgb8.r * 257);
	color.green = (unsigned short)(rgb8.g * 257);
	color.blue = (unsigned short)(rgb8.b * 257);
	color.flags = DoRed | DoGreen | DoBlue;
	XAllocColor(display, cmap, &color);
	swatch_set_color(swatch_ctx, color.pixel);
}

#pragma GCC diagnostic pop

/* --- Entry Focus Management --- */
/* Explicitly unfocus all entries and trigger validation */
static void unfocus_all_entries(void) {
	MiniEntry *entries[] = {entry_hsv, entry_hsl, entry_rgbf, entry_rgbi, entry_hex};
	for (int i = 0; i < 5; i++) {
		if (entries[i]) {
			// Unfocus the entry - this will trigger validation via FocusOut logic
			entry_focus(entries[i], 0);
		}
	}
}

/* Cycle focus to next/previous entry with Tab/Shift+Tab */
static void cycle_entry_focus(int forward) {
	MiniEntry *entries[] = {entry_hsv, entry_hsl, entry_rgbf, entry_rgbi, entry_hex};
	int current_index = -1;
	
	// Find currently focused entry
	for (int i = 0; i < 5; i++) {
		if (entries[i] && entry_is_focused_check(entries[i])) {
			current_index = i;
			break;
		}
	}
	
	// Calculate next index
	int next_index;
	if (current_index == -1) {
		// No entry focused, start at first
		next_index = forward ? 0 : 4;
	} else {
		if (forward) {
			// Forward: HSV(0) -> HSL(1) -> RGBf(2) -> RGBi(3) -> Hex(4) -> HSV(0)
			next_index = (current_index + 1) % 5;
		} else {
			// Backward: Hex(4) -> RGBi(3) -> RGBf(2) -> HSL(1) -> HSV(0) -> Hex(4)
			next_index = (current_index - 1 + 5) % 5;
		}
	}
	
	// Focus the next entry
	if (entries[next_index]) {
		entry_focus(entries[next_index], 1);
	}
}

/* --- Entry Change Callbacks --- */
static void entry_hsv_changed(MiniEntry *e, void *userdata) {
	(void)userdata;
	if (updating_from_callback) {
		return;
	}
	const char *text = entry_get_text(e);
	if (!text || !*text) {
		validate_entry_flash_invalid_and_restore(e);
		return;
	}
	double h, s, v;
	if (!parse_hsv(text, &h, &s, &v)) {
		validate_entry_flash_invalid_and_restore(e);
		return;
	}
	HSV hsv = {
		h, s, v
	};
	RGBf rgbf = hsv_to_rgb(hsv);
	// Convert to RGB8 and check if it actually changed to prevent rounding errors
	RGB8 rgb8 = rgbf_to_rgb8(rgbf);
	if (!rgb8_equal(rgb8, current_rgb8)) {
		validate_entry_flash_valid(e);
		updating_from_callback = 1;
		format_and_update_entries_from_rgbf(rgbf);
		updating_from_callback = 0;
	}
	// Valid but unchanged - no flash
}

static void entry_hsl_changed(MiniEntry *e, void *userdata) {
	(void)userdata;
	if (updating_from_callback) {
		return;
	}
	const char *text = entry_get_text(e);
	if (!text || !*text) {
		validate_entry_flash_invalid_and_restore(e);
		return;
	}
	double h, s, l;
	if (!parse_hsl(text, &h, &s, &l)) {
		validate_entry_flash_invalid_and_restore(e);
		return;
	}
	HSL hsl = {
		h, s, l
	};
	RGBf rgbf = hsl_to_rgb(hsl);
	// Convert to RGB8 and check if it actually changed to prevent rounding errors
	RGB8 rgb8 = rgbf_to_rgb8(rgbf);
	if (!rgb8_equal(rgb8, current_rgb8)) {
		validate_entry_flash_valid(e);
		updating_from_callback = 1;
		format_and_update_entries_from_rgbf(rgbf);
		updating_from_callback = 0;
	}
	// Valid but unchanged - no flash
}

static void entry_rgbf_changed(MiniEntry *e, void *userdata) {
	(void)userdata;
	if (updating_from_callback) {
		return;
	}
	const char *text = entry_get_text(e);
	if (!text || !*text) {
		validate_entry_flash_invalid_and_restore(e);
		return;
	}
	double r, g, b;
	if (!parse_rgbf(text, &r, &g, &b)) {
		validate_entry_flash_invalid_and_restore(e);
		return;
	}
	// Round to 3 decimals and clamp to [0,1] before comparing/updating
	double rr = fmin(1.0, fmax(0.0, floor(r * 1000.0 + 0.5) / 1000.0));
	double gg = fmin(1.0, fmax(0.0, floor(g * 1000.0 + 0.5) / 1000.0));
	double bb = fmin(1.0, fmax(0.0, floor(b * 1000.0 + 0.5) / 1000.0));
	RGBf rgbf = (RGBf) {
		rr, gg, bb
	};
	// Convert to RGB8 and check if it actually changed to prevent rounding errors
	RGB8 rgb8 = rgbf_to_rgb8(rgbf);
	if (!rgb8_equal(rgb8, current_rgb8)) {
		validate_entry_flash_valid(e);
		updating_from_callback = 1;
		format_and_update_entries_from_rgbf(rgbf);
		updating_from_callback = 0;
	}
	// Valid but unchanged - no flash
}

static void entry_rgbi_changed(MiniEntry *e, void *userdata) {
	(void)userdata;
	if (updating_from_callback) {
		return;
	}
	const char *text = entry_get_text(e);
	if (!text || !*text) {
		validate_entry_flash_invalid_and_restore(e);
		return;
	}
	int r, g, b;
	if (!parse_rgbi(text, &r, &g, &b)) {
		validate_entry_flash_invalid_and_restore(e);
		return;
	}
	RGB8 rgb8 = {
		(uint8_t)r, (uint8_t)g, (uint8_t)b
	};
	if (!rgb8_equal(rgb8, current_rgb8)) {
		validate_entry_flash_valid(e);
		updating_from_callback = 1;
		format_and_update_entries(rgb8);
		updating_from_callback = 0;
	}
	// Valid but unchanged - no flash
}

static void entry_hex_changed(MiniEntry *e, void *userdata) {
	(void)userdata;
	if (updating_from_callback) {
		return;
	}
	const char *text = entry_get_text(e);
	if (!text || !*text) {
		validate_entry_flash_invalid_and_restore(e);
		return;
	}
	RGB8 rgb8;
	if (!parse_hex(text, &rgb8)) {
		validate_entry_flash_invalid_and_restore(e);
		return;
	}
	if (!rgb8_equal(rgb8, current_rgb8)) {
		validate_entry_flash_valid(e);
		updating_from_callback = 1;
		format_and_update_entries(rgb8);
		updating_from_callback = 0;
	}
	// Valid but unchanged - no flash
}

/* --- Color Picker Event Handlers --- */
static void convert_pixel_color(void) {
	if (!zoom_color_picked_ctx(zoom_ctx)) {
		return;
	}
	unsigned long pixel = zoom_get_last_pixel_ctx(zoom_ctx);

	XColor color = {0};
	Colormap cmap = DefaultColormap(display, DefaultScreen(display));
	color.pixel = pixel;
	XQueryColor(display, cmap, &color);

	RGB8 rgb8 = {
		(uint8_t)(color.red / 257), (uint8_t)(color.green / 257), (uint8_t)(color.blue / 257)
	};

	updating_from_callback = 1;
	format_and_update_entries(rgb8);
	updating_from_callback = 0;

	Window swatch_win = swatch_get_window(swatch_ctx);
	XClearWindow(display, swatch_win);

	// Save zoom image when color is picked
	const char *home = getenv("HOME");
	if (!home) {
		home = ".";
	}
	char zoom_path[512];
	snprintf(zoom_path, sizeof(zoom_path), "%s/.config/pixelprism/last_zoom.dat", home);
	zoom_save_image(zoom_ctx, zoom_path);
}

/* css_to_pixel is now a wrapper for config_color_to_pixel */
static unsigned long css_to_pixel(ConfigColor c) {
	return config_color_to_pixel(display, DefaultScreen(display), c);
}

/* --- Window Visibility Management --- */
/**
 * hide_main_window - Hide main window (minimize to tray)
 *
 * Unmaps the main window effectively hiding it from view.
 * The tray icon remains visible for restoring the window.
 */
static void hide_main_window(void) {
	XUnmapWindow(display, main_window);
	XFlush(display);
}

/**
 * show_main_window - Show and raise main window
 *
 * Maps the main window and brings it to the front.
 * Called when tray icon is clicked.
 */
static void show_main_window(void) {
	// Map and raise the window
	XMapWindow(display, main_window);
	XRaiseWindow(display, main_window);
	XFlush(display);
}

/* --- Application Initialization --- */
static void init_entries(const MiniTheme *theme) {
	MiniEntryConfig cfg = {0};
	
	// HSV Entry
	cfg.kind = ENTRY_TEXT;
	cfg.x_pos = theme->entry_positions.entry_hsv_x;
	cfg.y_pos = theme->entry_positions.entry_hsv_y;
	cfg.width = theme->entry_positions.entry_hsv_width;
	cfg.padding = theme->entry_positions.entry_hsv_padding;
	cfg.border_width = theme->entry_positions.entry_hsv_border_width;
	cfg.border_radius = theme->entry_positions.entry_hsv_border_radius;
	cfg.max_length = theme->max_length.text;
	cfg.on_change = entry_hsv_changed;
	cfg.user_data = NULL;
	entry_hsv = entry_create(display, DefaultScreen(display), main_window, theme, &cfg, clipboard_ctx);
	
	// HSL Entry
	cfg.x_pos = theme->entry_positions.entry_hsl_x;
	cfg.y_pos = theme->entry_positions.entry_hsl_y;
	cfg.width = theme->entry_positions.entry_hsl_width;
	cfg.padding = theme->entry_positions.entry_hsl_padding;
	cfg.border_width = theme->entry_positions.entry_hsl_border_width;
	cfg.border_radius = theme->entry_positions.entry_hsl_border_radius;
	cfg.on_change = entry_hsl_changed;
	entry_hsl = entry_create(display, DefaultScreen(display), main_window, theme, &cfg, clipboard_ctx);
	
	// RGB Float Entry
	cfg.kind = ENTRY_FLOAT;
	cfg.x_pos = theme->entry_positions.entry_rgbf_x;
	cfg.y_pos = theme->entry_positions.entry_rgbf_y;
	cfg.width = theme->entry_positions.entry_rgbf_width;
	cfg.padding = theme->entry_positions.entry_rgbf_padding;
	cfg.border_width = theme->entry_positions.entry_rgbf_border_width;
	cfg.border_radius = theme->entry_positions.entry_rgbf_border_radius;
	cfg.max_length = theme->max_length.floating;
	cfg.on_change = entry_rgbf_changed;
	entry_rgbf = entry_create(display, DefaultScreen(display), main_window, theme, &cfg, clipboard_ctx);
	
	// RGB Integer Entry
	cfg.kind = ENTRY_INT;
	cfg.x_pos = theme->entry_positions.entry_rgbi_x;
	cfg.y_pos = theme->entry_positions.entry_rgbi_y;
	cfg.width = theme->entry_positions.entry_rgbi_width;
	cfg.padding = theme->entry_positions.entry_rgbi_padding;
	cfg.border_width = theme->entry_positions.entry_rgbi_border_width;
	cfg.border_radius = theme->entry_positions.entry_rgbi_border_radius;
	cfg.max_length = theme->max_length.integer;
	cfg.on_change = entry_rgbi_changed;
	entry_rgbi = entry_create(display, DefaultScreen(display), main_window, theme, &cfg, clipboard_ctx);
	
	// Hex Entry
	cfg.kind = ENTRY_HEX;
	cfg.x_pos = theme->entry_positions.entry_hex_x;
	cfg.y_pos = theme->entry_positions.entry_hex_y;
	cfg.width = theme->entry_positions.entry_hex_width;
	cfg.padding = theme->entry_positions.entry_hex_padding;
	cfg.border_width = theme->entry_positions.entry_hex_border_width;
	cfg.border_radius = theme->entry_positions.entry_hex_border_radius;
	cfg.max_length = theme->max_length.hex;
	cfg.on_change = entry_hex_changed;
	entry_hex = entry_create(display, DefaultScreen(display), main_window, theme, &cfg, clipboard_ctx);
}

static void init_labels(const MiniTheme *theme) {
	BaseTheme label_theme;
	strncpy(label_theme.font_family, theme->label.font_family, sizeof(label_theme.font_family) - 1);
	label_theme.font_family[sizeof(label_theme.font_family) - 1] = '\0';
	label_theme.font_size = theme->label.font_size;
	label_theme.fg_r = theme->label.fg.r;
	label_theme.fg_g = theme->label.fg.g;
	label_theme.fg_b = theme->label.fg.b;
	label_theme.fg_a = theme->label.fg.a;
	label_theme.bg_r = theme->label.bg.r;
	label_theme.bg_g = theme->label.bg.g;
	label_theme.bg_b = theme->label.bg.b;
	label_theme.bg_a = theme->label.bg.a;
	label_theme.border_r = theme->label.border.r;
	label_theme.border_g = theme->label.border.g;
	label_theme.border_b = theme->label.border.b;
	label_theme.border_a = theme->label.border.a;
	
	label_hsv = label_create(display, DefaultScreen(display), main_window, theme->label_positions.label_hsv_x, theme->label_positions.label_hsv_y, theme->label_positions.label_hsv_width, theme->label_positions.label_hsv_padding, theme->label_positions.label_hsv_border_width, theme->label_positions.label_hsv_border_radius, theme->label_positions.label_hsv_border_enabled, "HSV", &label_theme);
	label_hsl = label_create(display, DefaultScreen(display), main_window, theme->label_positions.label_hsl_x, theme->label_positions.label_hsl_y, theme->label_positions.label_hsl_width, theme->label_positions.label_hsl_padding, theme->label_positions.label_hsl_border_width, theme->label_positions.label_hsl_border_radius, theme->label_positions.label_hsl_border_enabled, "HSL", &label_theme);
	label_rgbf = label_create(display, DefaultScreen(display), main_window, theme->label_positions.label_rgbf_x, theme->label_positions.label_rgbf_y, theme->label_positions.label_rgbf_width, theme->label_positions.label_rgbf_padding, theme->label_positions.label_rgbf_border_width, theme->label_positions.label_rgbf_border_radius, theme->label_positions.label_rgbf_border_enabled, "0-1", &label_theme);
	label_rgbi = label_create(display, DefaultScreen(display), main_window, theme->label_positions.label_rgbi_x, theme->label_positions.label_rgbi_y, theme->label_positions.label_rgbi_width, theme->label_positions.label_rgbi_padding, theme->label_positions.label_rgbi_border_width, theme->label_positions.label_rgbi_border_radius, theme->label_positions.label_rgbi_border_enabled, "0-255", &label_theme);
	label_hex = label_create(display, DefaultScreen(display), main_window, theme->label_positions.label_hex_x, theme->label_positions.label_hex_y, theme->label_positions.label_hex_width, theme->label_positions.label_hex_padding, theme->label_positions.label_hex_border_width, theme->label_positions.label_hex_border_radius, theme->label_positions.label_hex_border_enabled, "Hex", &label_theme);
}

static void init_ui_widgets(const MiniTheme *theme) {
	// Create swatch
	swatch_ctx = swatch_create(display, main_window, theme->swatch_widget.width, theme->swatch_widget.height);
	swatch_set_position(swatch_ctx, theme->swatch_widget.swatch_x, theme->swatch_widget.swatch_y);
	swatch_set_background(swatch_ctx, css_to_pixel(theme->main.background));
	swatch_set_border(swatch_ctx, theme->swatch_widget.border_width, theme->swatch_widget.border_radius);
	
	// Create button
	button_ctx = button_create(display, main_window, &theme->button, theme->button_widget.width, theme->button_widget.height, theme->button_widget.padding, theme->button_widget.border_width, theme->button_widget.hover_border_width, theme->button_widget.active_border_width, theme->button_widget.border_radius);
	button_set_position(button_ctx, theme->button_widget.button_x, theme->button_widget.button_y);
	button_set_label(button_ctx, "Pick Color");
	
	// Create menubar
	MenuConfig menu_config = {
		.file_items = { "Exit" },
		.edit_items = { "Configuration", "Reset" },
		.about_items = { "PixelPrism" },
		.file_count = 1,
		.edit_count = 2,
		.about_count = 1
	};
	menubar = menubar_create_with_config(display, main_window, &theme->menubar, theme->menubar_widget.menubar_x, theme->menubar_widget.menubar_y, theme->menubar_widget.width, theme->menubar_widget.border_width, theme->menubar_widget.border_radius, theme->menubar_widget.padding, &menu_config);
	menubar_draw(menubar);
	
	// Create about window
	about_win = about_create(display, main_window, theme);
}

static void init_all_widgets(const MiniTheme *theme) {
	init_ui_widgets(theme);
	init_entries(theme);
	init_labels(theme);
}

static void setup_config_watching(void) {
	const char *home = getenv("HOME");
	if (!home) {
		home = ".";
	}
	inotify_fd = inotify_init1(IN_NONBLOCK);
	if (inotify_fd >= 0) {
		char dir_path[PATH_MAX];
		snprintf(dir_path, sizeof(dir_path), "%s/.config/pixelprism", home);
		watch_fd = inotify_add_watch(inotify_fd, dir_path, IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO);
		if (watch_fd < 0) {
			fprintf(stderr, "Warning: Could not watch config directory for changes\n");
			close(inotify_fd);
			inotify_fd = -1;
		}
	}
}

static void handle_inotify_events(void) {
	if (inotify_fd < 0) {
		return;
	}
	
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t len = read(inotify_fd, buf, sizeof(buf));
	if (len > 0) {
		static time_t last_reload_time = 0;
		const struct inotify_event *ievent;
		for (char *ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + ievent->len) {
			ievent = (const struct inotify_event *)ptr;
			if ((ievent->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO)) && ievent->len > 0) {
				if (strcmp(ievent->name, "pixelprism.conf") == 0) {
					// Debounce: only reload if 100ms has passed since last reload
					time_t now = time(NULL);
					if (now == last_reload_time) {
						continue; // Skip if within same second
					}
					last_reload_time = now;
					usleep(50000);
					reload_theme();
				}
			}
		}
	}
}

static void update_all_entry_blinks(void) {
	entry_update_blink(entry_hsv);
	entry_update_blink(entry_hsl);
	entry_update_blink(entry_rgbf);
	entry_update_blink(entry_rgbi);
	entry_update_blink(entry_hex);
	
	// Update validation flash timers (application-level coordination)
	update_validation_timers();
}

/* --- Theme Management --- */
static void apply_window_theme(void) {
	// Update main window size hints and dimensions
	XSizeHints *sizehint = XAllocSizeHints();
	if (sizehint) {
		sizehint->flags = PMaxSize | PMinSize;
		sizehint->min_width = sizehint->max_width = current_theme.main.main_width;
		sizehint->min_height = sizehint->max_height = current_theme.main.main_height;
		XSetWMNormalHints(display, main_window, sizehint);
		XFree(sizehint);
	}
	
	// Resize and update window background
	XResizeWindow(display, main_window, (unsigned)current_theme.main.main_width, (unsigned)current_theme.main.main_height);
	XSetWindowBackground(display, main_window, css_to_pixel(current_theme.main.background));
	XClearWindow(display, main_window);
	
	// Apply or remove always-on-top state
	Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
	Atom wm_state_above = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
	XEvent xev;
	memset(&xev, 0, sizeof(xev));
	xev.type = ClientMessage;
	xev.xclient.window = main_window;
	xev.xclient.message_type = wm_state;
	xev.xclient.format = 32;
	xev.xclient.data.l[0] = current_theme.always_on_top ? 1 : 0;
	xev.xclient.data.l[1] = (long)wm_state_above;
	xev.xclient.data.l[2] = 0;
	xev.xclient.data.l[3] = 1;
	xev.xclient.data.l[4] = 0;
	XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);
	XFlush(display);
}

static void apply_widget_themes(void) {
	// Update swatch
	if (swatch_ctx) {
		swatch_set_background(swatch_ctx, css_to_pixel(current_theme.main.background));
		swatch_resize(swatch_ctx, current_theme.swatch_widget.width, current_theme.swatch_widget.height);
		swatch_set_position(swatch_ctx, current_theme.swatch_widget.swatch_x, current_theme.swatch_widget.swatch_y);
		swatch_set_border(swatch_ctx, current_theme.swatch_widget.border_width, current_theme.swatch_widget.border_radius);
	}
	
	// Update button
	if (button_ctx) {
		button_set_theme(button_ctx, &current_theme.button);
		button_set_position(button_ctx, current_theme.button_widget.button_x, current_theme.button_widget.button_y);
	}
	
	// Update zoom
	if (zoom_ctx) {
		zoom_set_colors(zoom_ctx, config_color_to_pixel(display, DefaultScreen(display), current_theme.crosshair_color), config_color_to_pixel(display, DefaultScreen(display), current_theme.square_color));
		zoom_set_visibility(zoom_ctx, current_theme.zoom_widget.crosshair_show, current_theme.zoom_widget.square_show, current_theme.zoom_widget.crosshair_show_after_pick, current_theme.zoom_widget.square_show_after_pick);
	}
	
	// Update menubar
	if (menubar) {
		menubar_set_theme(menubar, &current_theme);
		menubar_set_position(menubar, current_theme.menubar_widget.menubar_x, current_theme.menubar_widget.menubar_y);
	}
	
	// Update about window
	if (about_win) {
		about_set_theme(about_win, &current_theme);
	}
	
	// Update tray menu
	if (tray_ctx) {
		tray_set_theme(tray_ctx, &current_theme);
	}
}

static void apply_entry_themes(void) {
	if (entry_hsv) {
		entry_resize_noflush(entry_hsv, current_theme.entry_positions.entry_hsv_width, 22);
		entry_set_theme_noflush(entry_hsv, &current_theme);
		entry_move_noflush(entry_hsv, current_theme.entry_positions.entry_hsv_x, current_theme.entry_positions.entry_hsv_y);
	}
	if (entry_hsl) {
		entry_resize_noflush(entry_hsl, current_theme.entry_positions.entry_hsl_width, 22);
		entry_set_theme_noflush(entry_hsl, &current_theme);
		entry_move_noflush(entry_hsl, current_theme.entry_positions.entry_hsl_x, current_theme.entry_positions.entry_hsl_y);
	}
	if (entry_rgbf) {
		entry_resize_noflush(entry_rgbf, current_theme.entry_positions.entry_rgbf_width, 22);
		entry_set_theme_noflush(entry_rgbf, &current_theme);
		entry_move_noflush(entry_rgbf, current_theme.entry_positions.entry_rgbf_x, current_theme.entry_positions.entry_rgbf_y);
	}
	if (entry_rgbi) {
		entry_resize_noflush(entry_rgbi, current_theme.entry_positions.entry_rgbi_width, 22);
		entry_set_theme_noflush(entry_rgbi, &current_theme);
		entry_move_noflush(entry_rgbi, current_theme.entry_positions.entry_rgbi_x, current_theme.entry_positions.entry_rgbi_y);
	}
	if (entry_hex) {
		entry_resize_noflush(entry_hex, current_theme.entry_positions.entry_hex_width, 22);
		entry_set_theme_noflush(entry_hex, &current_theme);
		entry_move_noflush(entry_hex, current_theme.entry_positions.entry_hex_x, current_theme.entry_positions.entry_hex_y);
		// Live-update hex case when hex_uppercase changes
		refresh_entry_from_current(entry_hex);
	}
}

static void apply_label_themes(void) {
	// Convert config to BaseTheme
	BaseTheme label_theme_reload;
	strncpy(label_theme_reload.font_family, current_theme.label.font_family, sizeof(label_theme_reload.font_family) - 1);
	label_theme_reload.font_family[sizeof(label_theme_reload.font_family) - 1] = '\0';
	label_theme_reload.font_size = current_theme.label.font_size;
	label_theme_reload.fg_r = current_theme.label.fg.r;
	label_theme_reload.fg_g = current_theme.label.fg.g;
	label_theme_reload.fg_b = current_theme.label.fg.b;
	label_theme_reload.fg_a = current_theme.label.fg.a;
	label_theme_reload.bg_r = current_theme.label.bg.r;
	label_theme_reload.bg_g = current_theme.label.bg.g;
	label_theme_reload.bg_b = current_theme.label.bg.b;
	label_theme_reload.bg_a = current_theme.label.bg.a;
	label_theme_reload.border_r = current_theme.label.border.r;
	label_theme_reload.border_g = current_theme.label.border.g;
	label_theme_reload.border_b = current_theme.label.border.b;
	label_theme_reload.border_a = current_theme.label.border.a;
	
	if (label_hsv) {
		label_set_theme(label_hsv, &label_theme_reload);
		label_move(label_hsv, current_theme.label_positions.label_hsv_x, current_theme.label_positions.label_hsv_y);
		label_resize(label_hsv, current_theme.label_positions.label_hsv_width, 0);
		label_set_geometry(label_hsv, current_theme.label_positions.label_hsv_padding, current_theme.label_positions.label_hsv_border_width, current_theme.label_positions.label_hsv_border_radius, current_theme.label_positions.label_hsv_border_enabled);
	}
	if (label_hsl) {
		label_set_theme(label_hsl, &label_theme_reload);
		label_move(label_hsl, current_theme.label_positions.label_hsl_x, current_theme.label_positions.label_hsl_y);
		label_resize(label_hsl, current_theme.label_positions.label_hsl_width, 0);
		label_set_geometry(label_hsl, current_theme.label_positions.label_hsl_padding, current_theme.label_positions.label_hsl_border_width, current_theme.label_positions.label_hsl_border_radius, current_theme.label_positions.label_hsl_border_enabled);
	}
	if (label_rgbf) {
		label_set_theme(label_rgbf, &label_theme_reload);
		label_move(label_rgbf, current_theme.label_positions.label_rgbf_x, current_theme.label_positions.label_rgbf_y);
		label_resize(label_rgbf, current_theme.label_positions.label_rgbf_width, 0);
		label_set_geometry(label_rgbf, current_theme.label_positions.label_rgbf_padding, current_theme.label_positions.label_rgbf_border_width, current_theme.label_positions.label_rgbf_border_radius, current_theme.label_positions.label_rgbf_border_enabled);
	}
	if (label_rgbi) {
		label_set_theme(label_rgbi, &label_theme_reload);
		label_move(label_rgbi, current_theme.label_positions.label_rgbi_x, current_theme.label_positions.label_rgbi_y);
		label_resize(label_rgbi, current_theme.label_positions.label_rgbi_width, 0);
		label_set_geometry(label_rgbi, current_theme.label_positions.label_rgbi_padding, current_theme.label_positions.label_rgbi_border_width, current_theme.label_positions.label_rgbi_border_radius, current_theme.label_positions.label_rgbi_border_enabled);
	}
	if (label_hex) {
		label_set_theme(label_hex, &label_theme_reload);
		label_move(label_hex, current_theme.label_positions.label_hex_x, current_theme.label_positions.label_hex_y);
		label_resize(label_hex, current_theme.label_positions.label_hex_width, 0);
		label_set_geometry(label_hex, current_theme.label_positions.label_hex_padding, current_theme.label_positions.label_hex_border_width, current_theme.label_positions.label_hex_border_radius, current_theme.label_positions.label_hex_border_enabled);
	}
}

static void reload_theme(void) {
	const char *home = getenv("HOME");
	if (!home) {
		home = ".";
	}
	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/.config/pixelprism/pixelprism.conf", home);
	if (config_load(&current_theme, config_path)) {
		fprintf(stderr, "Warning: Failed to reload config\n");
		return;
	}
	
	// Apply theme to all components
	apply_window_theme();
	apply_widget_themes();
	apply_entry_themes();
	
	// Critical: Sync and flush for atomic presentation
	XSync(display, False);
	XFlush(display);
	
	apply_label_themes();
	
	// Trigger swatch redraw
	if (swatch_ctx) {
		Window swatch_win = swatch_get_window(swatch_ctx);
		XEvent ev = {0};
		ev.type = Expose;
		ev.xexpose.window = swatch_win;
		ev.xexpose.count = 0;
		XSendEvent(display, swatch_win, False, ExposureMask, &ev);
	}
	XFlush(display);
}

void pixelprism(void) {
	/* Zero-initialize event structure to clear all padding bytes */
	XEvent event = {0};
	XGCValues xgcv;
	static Atom wm_delete_window;
	static Atom wm_protocols;
	unsigned long valuemask;
	unsigned long gcmask;
	int xpos = 0, ypos = 0;
	char *displayname = NULL;
	const char *home;
	if (!(display = XOpenDisplay(displayname))) {
		perror("Cannot open display");
		exit(-1);
	}
	screen = DefaultScreenOfDisplay(display);

	// Initialize clipboard system
	clipboard_ctx = clipboard_create(display);
	if (!clipboard_ctx) {
		fprintf(stderr, "Failed to create clipboard context\n");
		XCloseDisplay(display);
		exit(-1);
	}
	home = getenv("HOME");
	if (!home) {
		home = ".";
	}
	/* Zero-initialize theme structure to clear all padding bytes */
	MiniTheme theme = {0};
	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/.config/pixelprism/pixelprism.conf", home);

	// Check if config exists before loading
	int config_exists = 0;
	FILE *check_file = fopen(config_path, "r");
	if (check_file) {
		config_exists = 1;
		fclose(check_file);
	}
	if (!config_load(&theme, config_path)) {
		// Config loaded successfully (either from file or defaults)
		current_theme = theme;
		// Try to load window position from state file if remember-position is enabled
		if (theme.remember_position && state_load_window_position(&xpos, &ypos) == 0) {
			// Successfully loaded saved position
		}
		else {
			// Center the window on screen (first run or remember-position disabled)
			int screen_width = DisplayWidth(display, DefaultScreen(display));
			int screen_height = DisplayHeight(display, DefaultScreen(display));
			xpos = (screen_width - theme.main.main_width) / 2;
			ypos = (screen_height - theme.main.main_height) / 2;
		}
		// Restore saved color state
		current_rgbf.r = current_theme.current_color.r;
		current_rgbf.g = current_theme.current_color.g;
		current_rgbf.b = current_theme.current_color.b;
		current_rgb8 = rgbf_to_rgb8(current_rgbf);
		// Create config file if it doesn't exist
		if (!config_exists) {
			// Config file doesn't exist, create it with centered position
			char dir_path[PATH_MAX];
			snprintf(dir_path, sizeof(dir_path), "%s/.config/pixelprism", home);
			mkdir(dir_path, 0755);

			FILE *f = fopen(config_path, "w");
			if (f) {
				config_write_defaults_with_values(f, &current_theme);
				fclose(f);
			}
		}
	}
	else {
		// This should not happen with the updated config_load, but keep as fallback
		fprintf(stderr, "Warning: Failed to load config, using defaults\n");
		config_init(&theme);
		current_theme = theme;
	}
	setup_config_watching();
	
	/* Zero-initialize XGCValues to clear padding bytes */
	memset(&xgcv, 0, sizeof(xgcv));
	xgcv.plane_mask = AllPlanes;
	xgcv.subwindow_mode = IncludeInferiors;
	xgcv.function = GXcopy;

	valuemask = CWBackPixel | CWEventMask | CWBitGravity | CWBackingStore;
	gcmask = GCFunction | GCPlaneMask | GCSubwindowMode;

	zoom_gc = XCreateGC(display, RootWindowOfScreen(screen), gcmask, &xgcv);

	/* Zero-initialize XSetWindowAttributes to clear padding bytes */
	memset(&xsmwa, 0, sizeof(xsmwa));
	xsmwa.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | VisibilityChangeMask | FocusChangeMask | StructureNotifyMask;
	xsmwa.background_pixel = css_to_pixel(theme.main.background);
	xsmwa.bit_gravity = NorthWestGravity;
	xsmwa.backing_store = WhenMapped;

	XSizeHints *sizehint = XAllocSizeHints();
	sizehint->flags = PMaxSize | PMinSize;
	sizehint->min_width = sizehint->max_width = theme.main.main_width;
	sizehint->min_height = sizehint->max_height = theme.main.main_height;
	// Add position hints if remember-position is enabled
	if (theme.remember_position) {
		sizehint->flags |= PPosition | USPosition;
		sizehint->x = xpos;
		sizehint->y = ypos;
	}
	main_window = XCreateWindow(display, RootWindowOfScreen(screen), xpos, ypos, (unsigned)theme.main.main_width, (unsigned)theme.main.main_height, 1, DefaultDepthOfScreen(screen), InputOutput, DefaultVisualOfScreen(screen), valuemask, &xsmwa);

	XSetWMProperties(display, main_window, NULL, NULL, NULL, 0, sizehint, NULL, NULL);
	XFree(sizehint);

	zoom_ctx = zoom_create(display, main_window, 0, 0, 300, 300);
	zoom_window = zoom_get_window(zoom_ctx);
	// Set zoom overlay colors from config
	zoom_set_colors(zoom_ctx, config_color_to_pixel(display, DefaultScreen(display), theme.crosshair_color), config_color_to_pixel(display, DefaultScreen(display), theme.square_color));
	// Set zoom overlay visibility from config
	zoom_set_visibility(zoom_ctx, theme.zoom_widget.crosshair_show, theme.zoom_widget.square_show, theme.zoom_widget.crosshair_show_after_pick, theme.zoom_widget.square_show_after_pick);
	// Set zoom activation callback for button visual feedback
	zoom_set_activation_callback(zoom_ctx, on_zoom_activated, button_ctx);
	// Restore zoom magnification from state if available
	int saved_zoom_mag = 0;
	if (state_load_zoom_mag(&saved_zoom_mag)) {
		zoom_set_magnification_ctx(zoom_ctx, saved_zoom_mag);
	}

	XStoreName(display, main_window, "PixelPrism");
	// Create system tray icon with theme if enabled
	if (theme.show_tray_icon) {
		tray_ctx = tray_create(display, DefaultScreen(display), (const char **)pixelprism_icon_xpm, &theme.tray_menu, main_window);
		if (!tray_ctx) {
			fprintf(stderr, "Warning: Could not create system tray icon\n");
		}
	}
	XMapWindow(display, main_window);
	
	// Apply always-on-top if enabled
	if (theme.always_on_top) {
		Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
		Atom wm_state_above = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
		XChangeProperty(display, main_window, wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)&wm_state_above, 1);
	}
	
	// Create all widgets
	init_all_widgets(&theme);

	// Initialize to saved color (or black if first run)
	initialize_color_state();

	wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
	wm_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
	XSetWMProtocols(display, main_window, &wm_delete_window, 1);

	int x11_fd = ConnectionNumber(display);
	while (running) {
		// Handle config file changes
		if (inotify_fd >= 0) {
			fd_set read_fds;
			struct timeval timeout;
			FD_ZERO(&read_fds);
			FD_SET(x11_fd, &read_fds);
			FD_SET(inotify_fd, &read_fds);
			int max_fd = (x11_fd > inotify_fd) ? x11_fd : inotify_fd;
			timeout.tv_sec = 0;
			timeout.tv_usec = 50000;
			int ret = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
			if (ret > 0 && FD_ISSET(inotify_fd, &read_fds)) {
				handle_inotify_events();
			}
		}
		while (XPending(display)) {
			XNextEvent(display, &event);
			// Handle clipboard events first
			if (clipboard_handle_event(clipboard_ctx, &event)) {
				continue;
			}
			// Handle tray icon events
			if (tray_ctx) {
				int tray_result = tray_handle_event(tray_ctx, &event);
				if (tray_result == 1 || tray_result == 3) {
					// Left click (1) or Show Window menu item (3) - toggle window visibility
					XWindowAttributes attrs;
					if (XGetWindowAttributes(display, main_window, &attrs)) {
						if (attrs.map_state == IsViewable) {
							hide_main_window();
						}
						else {
							show_main_window();
						}
					}
					continue;
				}
				else if (tray_result == 2) {
					// Pick Color menu item - show window and trigger button action
					XWindowAttributes attrs;
					if (XGetWindowAttributes(display, main_window, &attrs)) {
						if (attrs.map_state != IsViewable) {
							show_main_window();
							XSync(display, False);
							usleep(100000); // Wait 100ms for window to fully raise
						}
					}
					// Ensure keyboard focus returns to our main window so
					// KeyPress events (arrows/Enter) reach zoom_handle_event
					XSetInputFocus(display, main_window, RevertToParent, CurrentTime);
					zoom_begin_selection_ctx(zoom_ctx);
					continue; // Skip rest of event handling this iteration
				}
				else if (tray_result == 4) {
					// Copy as Hex menu item - copy current color to clipboard
					char hex_str[8];
					format_hex(current_rgb8, hex_str, current_theme.hex_uppercase);
					// Remove # prefix if disabled
					if (!current_theme.hex_prefix && hex_str[0] == '#') {
						clipboard_set_text(clipboard_ctx, main_window, hex_str + 1, SELECTION_CLIPBOARD);
					}
					else {
						clipboard_set_text(clipboard_ctx, main_window, hex_str, SELECTION_CLIPBOARD);
					}
					continue;
				}
				else if (tray_result == 5) {
					// Exit menu item
					exit(0);
				}
			}
			// Handle about window events
			if (about_is_visible(about_win)) {
				if (about_handle_event(about_win, &event)) {
					continue;
				}
			}
			zoom_handle_event(zoom_ctx, &event);
			if (zoom_color_picked_ctx(zoom_ctx)) {
				convert_pixel_color();
				button_press = False;
				button_reset(button_ctx);
			}
			if (zoom_was_cancelled_ctx(zoom_ctx)) {
				button_press = False;
				button_reset(button_ctx);
			}
			// Handle menubar events
			int menubar_action = menubar_handle_event(menubar, &event);
			if (menubar_action == 0) {
				// File > Exit
				exit(0);
			}
			else if (menubar_action == 100) {
				// Edit > Configuration
				open_configuration();
			}
			else if (menubar_action == 101) {
				// Edit > Reset
				reset_to_black();
				initialize_color_state();
			}
			else if (menubar_action == 200) {
				// About > PixelPrism
				about_show(about_win);
			}
			switch (event.type) {
				case Expose:
					// Handle label expose events (initial display and window expose)
					if (label_hsv && event.xexpose.window == label_get_window(label_hsv)) {
						label_handle_expose(label_hsv, &event.xexpose);
					}
					else if (label_hsl && event.xexpose.window == label_get_window(label_hsl)) {
						label_handle_expose(label_hsl, &event.xexpose);
					}
					else if (label_rgbf && event.xexpose.window == label_get_window(label_rgbf)) {
						label_handle_expose(label_rgbf, &event.xexpose);
					}
					else if (label_rgbi && event.xexpose.window == label_get_window(label_rgbi)) {
						label_handle_expose(label_rgbi, &event.xexpose);
					}
					else if (label_hex && event.xexpose.window == label_get_window(label_hex)) {
						label_handle_expose(label_hex, &event.xexpose);
					}
				break;

				case ClientMessage:
					if ((event.xclient.message_type == wm_protocols) && ((Atom)event.xclient.data.l[0] == wm_delete_window)) {
						// If tray icon exists AND minimize-to-tray is enabled, minimize to tray instead of exiting
						if (tray_ctx && current_theme.minimize_to_tray) {
							hide_main_window();
						}
						else {
							exit(0);
						}
					}
				break;

				case KeyPress: {
			KeySym ks = XkbKeycodeToKeysym(display, (KeyCode)event.xkey.keycode, 0, 0);
			if (ks == XK_Escape) {
				exit(0);
			}
			else if (ks == XK_Tab || ks == XK_ISO_Left_Tab) {
				// Tab or Shift+Tab to cycle focus between entries
				int forward = !(event.xkey.state & ShiftMask);
				cycle_entry_focus(forward);
			}
			// Note: Ctrl+Alt+Z is now handled by zoom widget itself
			}
			break;

				case FocusIn:
					if (event.xfocus.window == main_window) {
						entry_handle_window_focus(entry_hsv, 1);
						entry_handle_window_focus(entry_hsl, 1);
						entry_handle_window_focus(entry_rgbf, 1);
						entry_handle_window_focus(entry_rgbi, 1);
						entry_handle_window_focus(entry_hex, 1);
					}
				break;

				case FocusOut:
					if (event.xfocus.window == main_window) {
						// Unfocus and validate all entries when window loses focus
						unfocus_all_entries();
						entry_handle_window_focus(entry_hsv, 0);
						entry_handle_window_focus(entry_hsl, 0);
						entry_handle_window_focus(entry_rgbf, 0);
						entry_handle_window_focus(entry_rgbi, 0);
						entry_handle_window_focus(entry_hex, 0);
					}
				break;

				case ConfigureNotify:
				// Don't track position during runtime - only query on exit
			break;
			}

			int button_result = button_handle_event(button_ctx, &event);
			if (button_result == 2) {
				button_press = True;
				button_set_pressed(button_ctx, 1);
				zoom_begin_selection_ctx(zoom_ctx);
			}
			// Menu widget now handles auto-hiding internally
			swatch_handle_event(swatch_ctx, &event, main_window);

			// Track if any entry handled the event (to know if we clicked an entry)
			int entry_handled = 0;
			entry_handled |= entry_handle_event(entry_hsv, &event);
			entry_handled |= entry_handle_event(entry_hsl, &event);
			entry_handled |= entry_handle_event(entry_rgbf, &event);
			entry_handled |= entry_handle_event(entry_rgbi, &event);
			entry_handled |= entry_handle_event(entry_hex, &event);
		
			// Unfocus all entries on button press ONLY if we didn't click an entry
			// This triggers validation when clicking non-entry widgets
			if (event.type == ButtonPress && !entry_handled) {
				unfocus_all_entries();
			}
		}
		update_all_entry_blinks();
	}
}

/**
 * cleanup_all_widgets - Free all allocated widget resources
 *
 * This function is called on program exit to properly free all allocated
 * memory for widgets. Prevents memory leaks by calling destroy functions
 * for all created widgets. Also auto-saves config if there are unsaved changes.
 */
static void cleanup_all_widgets(void) {
	// Save window position if remember-position is enabled
	if (current_theme.remember_position && main_window) {
		// Query the parent (frame) window position
		Window root, parent, *children;
		unsigned int nchildren;
		if (XQueryTree(display, main_window, &root, &parent, &children, &nchildren)) {
			if (children) XFree(children);
			if (parent && parent != root) {
				// Get parent frame position in root coordinates
				XWindowAttributes attrs;
				if (XGetWindowAttributes(display, parent, &attrs)) {
					state_save_window_position(attrs.x, attrs.y);
				}
			}
		}
	}

	// Save last picked color to state file
	RGB8 save_rgb8 = rgbf_to_rgb8(current_rgbf);
	char hex[8];
	rgb8_to_hex(save_rgb8, hex);
	state_save_last_color(hex);

	// Only save config if configuration has unsaved changes
	if (config_has_unsaved_changes(&current_theme)) {
		const char *home = getenv("HOME");
		if (!home) {
			home = ".";
		}
		char config_path[512];
		snprintf(config_path, sizeof(config_path), "%s/.config/pixelprism/pixelprism.conf", home);

		FILE *f = fopen(config_path, "w");
		if (f) {
			config_write_defaults_with_values(f, &current_theme);
			fclose(f);
			config_mark_saved(&current_theme);
		}
	}
	// Destroy all entry widgets
	if (entry_hsv) {
		entry_destroy(entry_hsv);
	}
	if (entry_hsl) {
		entry_destroy(entry_hsl);
	}
	if (entry_rgbf) {
		entry_destroy(entry_rgbf);
	}
	if (entry_rgbi) {
		entry_destroy(entry_rgbi);
	}
	if (entry_hex) {
		entry_destroy(entry_hex);
	}
	// Destroy all label widgets
	if (label_hsv) {
		label_destroy(label_hsv);
	}
	if (label_hsl) {
		label_destroy(label_hsl);
	}
	if (label_rgbf) {
		label_destroy(label_rgbf);
	}
	if (label_rgbi) {
		label_destroy(label_rgbi);
	}
	if (label_hex) {
		label_destroy(label_hex);
	}
	// Destroy button widget
	if (button_ctx) {
		button_destroy(button_ctx);
	}
	// Destroy menubar widget
	if (menubar) {
		menubar_destroy(menubar);
	}
	// Destroy swatch widget
	if (swatch_ctx) {
		swatch_destroy(swatch_ctx);
	}
	// Destroy zoom widget
	if (zoom_ctx) {
		int zoom_mag = zoom_get_magnification_ctx(zoom_ctx);
		state_save_zoom_mag(zoom_mag);
		zoom_destroy(zoom_ctx);
	}
	// Destroy about window
	if (about_win) {
		about_destroy(about_win);
	}
	// Cleanup clipboard
	if (clipboard_ctx) {
		clipboard_destroy(clipboard_ctx);
		clipboard_ctx = NULL;
	}
	// Destroy tray icon
	if (tray_ctx) {
		tray_destroy(tray_ctx);
	}
	// Close inotify file descriptors
	if (inotify_fd >= 0) {
		close(inotify_fd);
	}
	// Free X11 graphics context
	if (zoom_gc) {
		XFreeGC(display, zoom_gc);
		zoom_gc = NULL; // Free graphics context
	}
	// Close X display connection - this automatically frees most resources
	if (display) {
		XCloseDisplay(display);
		display = NULL;
	}
	// Note: FcFini() intentionally not called as it causes assertion failures
	// when fontconfig's internal cache state isn't fully cleared. XCloseDisplay()
	// handles cleanup of font resources. Remaining fontconfig leaks are internal
	// to the library and unavoidable without library fixes.
}

/**
 * signal_handler - Handle SIGTERM and SIGINT signals
 * @sig Signal number
 *
 * Sets flag to gracefully exit main loop and trigger cleanup.
 */
static void signal_handler(int sig) {
	(void)sig;
	running = 0;
}


/* ============================================================================
 *                    APPLICATION CONFIGURATION SYSTEM                         
 * ============================================================================
 * This section contains PixelPrism-specific configuration management code
 * that was moved from config.c to eliminate circular dependencies with widgets.
 * 
 * config.c now contains ONLY pure utilities (parse_color, config_open_font, etc.)
 * that widgets can safely use without creating circular dependencies.
 */

/* ========== APPLICATION DETECTION ========== */

/* Smart application detection */
static void detect_best_editor(char *path, size_t size) {
	const char *editors[] = {
		"code", // VS Code
		"gedit", // GNOME Text Editor
		"kate", // KDE Advanced Text Editor
		"mousepad", // Xfce Text Editor
		"leafpad", // Lightweight GTK+ editor
		"geany", // Fast, lightweight IDE
		"subl", // Sublime Text
		"atom", // Atom editor
		"vim", // Vim (terminal)
		"nano", // Nano (terminal, fallback)
		NULL
	};
	for (int i = 0; editors[i]; i++) {
		char cmd[256];
		snprintf(cmd, sizeof(cmd), "which %s >/dev/null 2>&1", editors[i]);
		if (system(cmd) == 0) {
			snprintf(path, size, "/usr/bin/%s", editors[i]);
			return;
		}
	}
	strncpy(path, "/usr/bin/nano", size); // Ultimate fallback
}

static void detect_best_browser(char *path, size_t size) {
	// Prefer xdg-open if available (uses user's default browser)
	if (system("which xdg-open >/dev/null 2>&1") == 0) {
		strncpy(path, "/usr/bin/xdg-open", size);
		return;
	}
	const char *browsers[] = {
		"firefox",
		"google-chrome",
		"chromium-browser",
		"chromium",
		"opera",
		"brave",
		"waterfox",
		"palemoon",
		"seamonkey",
		NULL
	};
	for (int i = 0; browsers[i]; i++) {
		char cmd[256];
		snprintf(cmd, sizeof(cmd), "which %s >/dev/null 2>&1", browsers[i]);
		if (system(cmd) == 0) {
			snprintf(path, size, "/usr/bin/%s", browsers[i]);
			return;
		}
	}
	strncpy(path, "/usr/bin/firefox", size); // Ultimate fallback
}

/* ========== CONFIGURATION CONSTANTS ========== */

#define DEFAULT_FONT "DejaVu Sans"
#define CONFIG_SUBPATH ".config/pixelprism/pixelprism.conf"
#define FONT_FAMILY_MAX_LEN 128
#define CONFIG_FILENAME "pixelprism.conf"
#define CONFIG_DIRNAME ".config/pixelprism"

static BorderMode current_border_mode = BORDER_MODE_COMPLEMENTARY;

/* ========== SECTION HANDLER IMPLEMENTATIONS ========== */

/* --- Button Section Handlers --- */
static void button_section_init(PixelPrismConfig *cfg) {
	if (!cfg) {
		return;
	}
	button_config_init_defaults(&cfg->button);
}

static int button_section_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg) {
		return 0;
	}
	return button_config_parse(&cfg->button, key, value);
}

static void button_section_write(FILE *f, const PixelPrismConfig *cfg) {
	if (!cfg || !f) {
		return;
	}
	button_config_write(f, &cfg->button);
}

static const ConfigSectionHandler button_section_handler = {
	.section = "button",
	.init_defaults = button_section_init,
	.parse = button_section_parse,
	.write = button_section_write,
};

/* --- Button Widget Section Handlers --- */
static void button_widget_section_init(PixelPrismConfig *cfg) {
	if (cfg) {
		button_widget_config_init_defaults(cfg);
	}
}

static int button_widget_section_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg || !key || !value) {
		return 0;
	}
	return button_widget_config_parse(cfg, key, value);
}

// button-widget section functions called directly (not via registry)

/* --- Entry Section Handlers --- */
static void entry_text_init(PixelPrismConfig *cfg) {
	if (cfg) {
		entry_config_init_defaults(&cfg->entry_text, "entry-text");
	}
}

static int entry_text_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg) {
		return 0;
	}
	entry_config_parse(&cfg->entry_text, key, value);
	return 1;
}

static void entry_text_write(FILE *f, const PixelPrismConfig *cfg) {
	if (!cfg || !f) {
		return;
	}
	entry_config_write(f, &cfg->entry_text, "entry-text");
}

static const ConfigSectionHandler entry_text_handler = {
	.section = "entry-text",
	.init_defaults = entry_text_init,
	.parse = entry_text_parse,
	.write = entry_text_write,
};

static void entry_int_init(PixelPrismConfig *cfg) {
	if (cfg) {
		entry_config_init_defaults(&cfg->entry_int, "entry-int");
	}
}

static int entry_int_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg) {
		return 0;
	}
	entry_config_parse(&cfg->entry_int, key, value);
	return 1;
}

static void entry_int_write(FILE *f, const PixelPrismConfig *cfg) {
	if (!cfg || !f) {
		return;
	}
	entry_config_write(f, &cfg->entry_int, "entry-int");
}

static const ConfigSectionHandler entry_int_handler = {
	.section = "entry-int",
	.init_defaults = entry_int_init,
	.parse = entry_int_parse,
	.write = entry_int_write,
};

static void entry_float_init(PixelPrismConfig *cfg) {
	if (cfg) {
		entry_config_init_defaults(&cfg->entry_float, "entry-float");
	}
}

static int entry_float_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg) {
		return 0;
	}
	entry_config_parse(&cfg->entry_float, key, value);
	return 1;
}

static void entry_float_write(FILE *f, const PixelPrismConfig *cfg) {
	if (!cfg || !f) {
		return;
	}
	entry_config_write(f, &cfg->entry_float, "entry-float");
}

static const ConfigSectionHandler entry_float_handler = {
	.section = "entry-float",
	.init_defaults = entry_float_init,
	.parse = entry_float_parse,
	.write = entry_float_write,
};

static void entry_hex_init(PixelPrismConfig *cfg) {
	if (cfg) {
		entry_config_init_defaults(&cfg->entry_hex, "entry-hex");
	}
}

static int entry_hex_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg) {
		return 0;
	}
	entry_config_parse(&cfg->entry_hex, key, value);
	return 1;
}

static void entry_hex_write(FILE *f, const PixelPrismConfig *cfg) {
	if (!cfg || !f) {
		return;
	}
	entry_config_write(f, &cfg->entry_hex, "entry-hex");
}

static const ConfigSectionHandler entry_hex_handler = {
	.section = "entry-hex",
	.init_defaults = entry_hex_init,
	.parse = entry_hex_parse,
	.write = entry_hex_write,
};

/* --- Menu Section Handlers --- */
static void menu_section_init(PixelPrismConfig *cfg) {
	if (cfg) {
		menu_config_init_defaults(&cfg->menu, "context-menu");
	}
}

static int menu_section_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg) {
		return 0;
	}
	menu_config_parse(&cfg->menu, key, value);
	return 1;
}

static void menu_section_write(FILE *f, const PixelPrismConfig *cfg) {
	if (!cfg || !f) {
		return;
	}
	menu_config_write(f, &cfg->menu, "context-menu");
}

static const ConfigSectionHandler menu_section_handler = {
	.section = "context-menu",
	.init_defaults = menu_section_init,
	.parse = menu_section_parse,
	.write = menu_section_write,
};

static void menubar_section_init(PixelPrismConfig *cfg) {
	if (cfg) {
		menu_config_init_defaults(&cfg->menubar, "menubar");
	}
}

static int menubar_section_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg) {
		return 0;
	}
	menu_config_parse(&cfg->menubar, key, value);
	return 1;
}

static void menubar_section_write(FILE *f, const PixelPrismConfig *cfg) {
	if (!cfg || !f) {
		return;
	}
	menu_config_write(f, &cfg->menubar, "menubar");
}

static const ConfigSectionHandler menubar_section_handler = {
	.section = "menubar",
	.init_defaults = menubar_section_init,
	.parse = menubar_section_parse,
	.write = menubar_section_write,
};

/* --- Menubar Widget Section Handlers --- */
static void menubar_widget_section_init(PixelPrismConfig *cfg) {
	if (cfg) {
		menubar_widget_config_init_defaults(cfg);
	}
}

static int menubar_widget_section_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg) {
		return 0;
	}
	menubar_widget_config_parse(cfg, key, value);
	return 1;
}

// menubar-widget section functions called directly (not via registry)

/* --- Swatch Section Handlers --- */
static void swatch_section_init(PixelPrismConfig *cfg) {
	if (cfg) {
		swatch_config_init_defaults(cfg);
	}
}

static int swatch_section_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg) {
		return 0;
	}
	swatch_config_parse(cfg, key, value);
	return 1;
}

static void swatch_section_write(FILE *f, const PixelPrismConfig *cfg) {
	if (!cfg || !f) {
		return;
	}
	swatch_config_write(f, cfg);
}

static const ConfigSectionHandler swatch_section_handler = {
	.section = "swatch",
	.init_defaults = swatch_section_init,
	.parse = swatch_section_parse,
	.write = swatch_section_write,
};

/* --- Swatch Widget Section Handlers --- */
static void swatch_widget_section_init(PixelPrismConfig *cfg) {
	if (cfg) {
		swatch_widget_config_init_defaults(cfg);
	}
}

static int swatch_widget_section_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg) {
		return 0;
	}
	swatch_widget_config_parse(cfg, key, value);
	return 1;
}

// swatch-widget section functions called directly (not via registry)

/* --- Tray Section Handlers --- */
static void tray_section_init(PixelPrismConfig *cfg) {
	if (cfg) {
		tray_config_init_defaults(cfg);
	}
}

static int tray_section_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg) {
		return 0;
	}
	tray_config_parse(cfg, key, value);
	return 1;
}

static void tray_section_write(FILE *f, const PixelPrismConfig *cfg) {
	if (!cfg || !f) {
		return;
	}
	tray_config_write(f, cfg);
}

static const ConfigSectionHandler tray_section_handler = {
	.section = "tray-menu",
	.init_defaults = tray_section_init,
	.parse = tray_section_parse,
	.write = tray_section_write,
};

/* --- Zoom Section Handlers --- */
static void zoom_section_init(PixelPrismConfig *cfg) {
	if (!cfg) {
		return;
	}
	cfg->crosshair_color = (ConfigColor){0.0, 1.0, 0.0, 1.0};
	cfg->square_color = (ConfigColor){1.0, 0.0, 0.0, 1.0};
}

static void zoom_widget_section_init(PixelPrismConfig *cfg) {
	if (!cfg) {
		return;
	}
	cfg->zoom_widget.crosshair_show = 1;
	cfg->zoom_widget.square_show = 1;
	cfg->zoom_widget.crosshair_show_after_pick = 0;
	cfg->zoom_widget.square_show_after_pick = 1;
}

static int zoom_section_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg) {
		return 0;
	}
	if (strcmp(key, "crosshair-color") == 0) {
		cfg->crosshair_color = parse_color(value);
		return 1;
	}
	if (strcmp(key, "square-color") == 0) {
		cfg->square_color = parse_color(value);
		return 1;
	}
	return 0;
}

static int zoom_widget_section_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg) {
		return 0;
	}
	if (strcmp(key, "crosshair-show") == 0) {
		cfg->zoom_widget.crosshair_show = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
		return 1;
	}
	if (strcmp(key, "square-show") == 0) {
		cfg->zoom_widget.square_show = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
		return 1;
	}
	if (strcmp(key, "crosshair-show-after-pick") == 0) {
		cfg->zoom_widget.crosshair_show_after_pick = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
		return 1;
	}
	if (strcmp(key, "square-show-after-pick") == 0) {
		cfg->zoom_widget.square_show_after_pick = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
		return 1;
	}
	return 0;
}

static void zoom_section_write(FILE *f, const PixelPrismConfig *cfg) {
	if (!cfg || !f) {
		return;
	}
	fprintf(f, "[zoom]\n");
	fprintf(f, "crosshair-color = #%02X%02X%02X\n",
	        (int)(cfg->crosshair_color.r * 255),
	        (int)(cfg->crosshair_color.g * 255),
	        (int)(cfg->crosshair_color.b * 255));
	fprintf(f, "square-color = #%02X%02X%02X\n\n",
	        (int)(cfg->square_color.r * 255),
	        (int)(cfg->square_color.g * 255),
	        (int)(cfg->square_color.b * 255));
}

static const ConfigSectionHandler zoom_section_handler = {
	.section = "zoom",
	.init_defaults = zoom_section_init,
	.parse = zoom_section_parse,
	.write = zoom_section_write,
};

// zoom-widget section functions called directly (not via registry)

/* --- Label Section Handlers --- */
static void label_section_init(PixelPrismConfig *cfg) {
	if (!cfg) {
		return;
	}
	ConfigColor default_fg = cfg->label.fg;
	ConfigColor default_bg = cfg->label.bg;
	ConfigColor default_border = cfg->label.border;
	label_config_init_defaults((LabelConfig *)&cfg->label, default_fg, default_bg, default_border);
}

static int label_section_parse(PixelPrismConfig *cfg, const char *key, const char *value) {
	if (!cfg) {
		return 0;
	}
	label_config_parse((LabelConfig *)&cfg->label, key, value);
	return 1;
}

static void label_section_write(FILE *f, const PixelPrismConfig *cfg) {
	if (!cfg || !f) {
		return;
	}
	label_config_write(f, (const LabelConfig *)&cfg->label);
}

static const ConfigSectionHandler label_section_handler = {
	.section = "label",
	.init_defaults = label_section_init,
	.parse = label_section_parse,
	.write = label_section_write,
};

/* ========== SECTION HANDLER REGISTRATION ========== */

/*
 * Register every built-in widget/config section handler.
 *
 * Centralizes the list so init/load/write paths can simply iterate the
 * registry without hardcoding widget knowledge elsewhere.
 */
static void config_register_builtin_sections(void) {
	config_registry_reset();
	// Register ONLY styling sections (top half) - widget sections are manually written at bottom
	// Alphabetical order: button, context-menu, entry-*, label, menubar, swatch, tray-menu, zoom
	config_registry_register(&button_section_handler);
	config_registry_register(&menu_section_handler);        // [context-menu]
	config_registry_register(&entry_float_handler);
	config_registry_register(&entry_hex_handler);
	config_registry_register(&entry_int_handler);
	config_registry_register(&entry_text_handler);
	config_registry_register(&label_section_handler);
	config_registry_register(&menubar_section_handler);
	config_registry_register(&swatch_section_handler);
	config_registry_register(&tray_section_handler);
	config_registry_register(&zoom_section_handler);
}

static void registry_init_cb(const ConfigSectionHandler *handler, void *user_data) {
	PixelPrismConfig *cfg = (PixelPrismConfig *)user_data;
	if (handler && handler->init_defaults) {
		handler->init_defaults(cfg);
	}
}

struct ConfigWriteContext {
	FILE *f;
	const PixelPrismConfig *cfg;
};

static void registry_write_cb(const ConfigSectionHandler *handler, void *user_data) {
	struct ConfigWriteContext *ctx = (struct ConfigWriteContext *)user_data;
	if (!ctx || !handler || !handler->write) {
		return;
	}
	handler->write(ctx->f, ctx->cfg);
}

/* ========== CONFIGURATION INITIALIZATION ========== */

/* Initialize configuration with defaults */
/*
 * Populate a configuration struct with default values. This seeds every widget
 * block via the registry, then fills in global behavior/theme defaults.
 */
void config_init_defaults(PixelPrismConfig *cfg) {
	if (!cfg) {
		return;
	}
	/* Zero-initialize entire configuration structure to clear padding bytes */
	memset(cfg, 0, sizeof(PixelPrismConfig));
	
	// Register all builtin styling sections
	config_register_builtin_sections();
	
	// Initialize all registered section handlers (styling sections)
	config_registry_for_each(registry_init_cb, cfg);
	
	// Manually initialize widget sections (not in registry - written at bottom of config)
	button_widget_section_init(cfg);
	menubar_widget_section_init(cfg);
	swatch_widget_section_init(cfg);
	zoom_widget_section_init(cfg);

	// Set swatch border mode to the current global
	cfg->config_changed = 0; // Start with no unsaved changes

	// Default current color (black - user can override)
	cfg->current_color = (ConfigColor) {
		0.0, 0.0, 0.0, 1.0
	}; // #000000

	// Default colors - GTK Adwaita light theme
	ConfigColor bg = {
		0.965, 0.961, 0.957, 1.0
	}; // #F6F5F4 (warm light gray)
	ConfigColor fg = {
		0.180, 0.204, 0.212, 1.0
	}; // #2E3436 (dark slate)
	ConfigColor br = {
		0.804, 0.780, 0.761, 1.0
	}; // #CDC7C2 (light gray border)
	//ConfigColor hover_green = {
	//	0.0, 0.5, 0.0, 1.0
	//}; // #008000
	//ConfigColor active_red = {
	//	0.5, 0.0, 0.0, 1.0
	//}; // #800000

	// Entry defaults
	entry_config_init_defaults(&cfg->entry_text, "entry-text");
	entry_config_init_defaults(&cfg->entry_int, "entry-int");
	entry_config_init_defaults(&cfg->entry_float, "entry-float");
	entry_config_init_defaults(&cfg->entry_hex, "entry-hex");

// Menu/Menubar defaults
	menu_config_init_defaults(&cfg->menu, "menu");
	menu_config_init_defaults(&cfg->menubar, "menubar");

	// Button defaults - delegated to button widget
	//button_config_init_defaults(&cfg->button, fg, bg, br, hover_green, active_red);
	// Label defaults - delegated to label widget
	label_config_init_defaults((LabelConfig*)&cfg->label, fg, bg, br);

// Change tracking
	cfg->config_changed = 0; // Start with no unsaved changes

// Main window defaults
	cfg->main.background = bg;
	strncpy(cfg->main.font_family, DEFAULT_FONT, FONT_FAMILY_MAX_LEN - 1);
	cfg->main.font_size = 14;
	cfg->main.text_color = fg;
	cfg->main.link_color = (ConfigColor) {
		0.110, 0.443, 0.847, 1.0
	}; // #1C71D8 GTK accent blue
	cfg->main.link_underline = 1; // Underline links by default

// Swatch defaults
	swatch_config_init_defaults(cfg);

	// Tray menu configuration - match menu/context styling
	strncpy(cfg->tray_menu.font_family, DEFAULT_FONT, sizeof(cfg->tray_menu.font_family) - 1);
	cfg->tray_menu.font_size = 14;
	cfg->tray_menu.fg = fg; // #2E3436
	cfg->tray_menu.bg = bg; // #F6F5F4
	cfg->tray_menu.hover_bg = (ConfigColor) {
		0.882, 0.871, 0.859, 1.0
	}; // #E1DEDB Match menu hover
	cfg->tray_menu.border = br; // #CDC7C2
	
	// Tray menu widget geometry
	cfg->tray_menu_widget.padding = 2;
	cfg->tray_menu_widget.border_width = 1;
	cfg->tray_menu_widget.border_radius = 4;

	// Main background
	cfg->swatch_border_mode = BORDER_MODE_COMPLEMENTARY;
	current_border_mode = cfg->swatch_border_mode;

// Behavior defaults
	cfg->hex_uppercase = 1; // Default to uppercase
	cfg->cursor_blink_ms = 700;
	cfg->cursor_color = (ConfigColor){0.208, 0.518, 0.894, 1.0}; // #3584E4 GTK cursor blue
	cfg->cursor_thickness = 1;

// Window Management defaults
	cfg->remember_position = 1;
	cfg->always_on_top = 1;
	cfg->show_tray_icon = 1;
	cfg->minimize_to_tray = 1;

// Clipboard defaults
	cfg->auto_copy = 0;
	strncpy(cfg->auto_copy_format, "hex", 7); // Options: hex, hsv, hsl, rgb, rgbi
	cfg->hex_prefix = 1; // Include # symbol when copying hex
	cfg->auto_copy_primary = 1; // Auto-copy selection to PRIMARY (X11 native behavior)

// Zoom defaults - colors and visibility
	cfg->crosshair_color = (ConfigColor) {
		0.0, 1.0, 0.0, 1.0
	}; // #00ff00 Green crosshair
	cfg->square_color = (ConfigColor) {
		1.0, 0.0, 0.0, 1.0
	}; // #ff0000 Red square
	cfg->zoom_widget.crosshair_show = 1;
	cfg->zoom_widget.square_show = 1;
	cfg->zoom_widget.crosshair_show_after_pick = 0;
	cfg->zoom_widget.square_show_after_pick = 1;

// Entry instance geometry (5 visual entries)
	cfg->entry_positions.entry_hsv_x = 383;
	cfg->entry_positions.entry_hsv_y = 40;
	cfg->entry_positions.entry_hsv_width = 197;
	cfg->entry_positions.entry_hsv_padding = 4;
	cfg->entry_positions.entry_hsv_border_width = 1;
	cfg->entry_positions.entry_hsv_border_radius = 4;
	cfg->entry_positions.entry_hsl_x = 383;
	cfg->entry_positions.entry_hsl_y = 75;
	cfg->entry_positions.entry_hsl_width = 197;
	cfg->entry_positions.entry_hsl_padding = 4;
	cfg->entry_positions.entry_hsl_border_width = 1;
	cfg->entry_positions.entry_hsl_border_radius = 4;
	cfg->entry_positions.entry_rgbf_x = 383;
	cfg->entry_positions.entry_rgbf_y = 110;
	cfg->entry_positions.entry_rgbf_width = 197;
	cfg->entry_positions.entry_rgbf_padding = 4;
	cfg->entry_positions.entry_rgbf_border_width = 1;
	cfg->entry_positions.entry_rgbf_border_radius = 4;
	cfg->entry_positions.entry_rgbi_x = 383;
	cfg->entry_positions.entry_rgbi_y = 145;
	cfg->entry_positions.entry_rgbi_width = 197;
	cfg->entry_positions.entry_rgbi_padding = 4;
	cfg->entry_positions.entry_rgbi_border_width = 1;
	cfg->entry_positions.entry_rgbi_border_radius = 4;
	cfg->entry_positions.entry_hex_x = 383;
	cfg->entry_positions.entry_hex_y = 180;
	cfg->entry_positions.entry_hex_width = 197;
	cfg->entry_positions.entry_hex_padding = 4;
	cfg->entry_positions.entry_hex_border_width = 1;
	cfg->entry_positions.entry_hex_border_radius = 4;

// Label instance positions and geometry (5 visual labels)
	cfg->label_positions.label_hsv_x = 310;
	cfg->label_positions.label_hsv_y = 40;
	cfg->label_positions.label_hsv_width = 60;
	cfg->label_positions.label_hsv_padding = 4;
	cfg->label_positions.label_hsv_border_width = 1;
	cfg->label_positions.label_hsv_border_radius = 0;
	cfg->label_positions.label_hsv_border_enabled = 0;
	cfg->label_positions.label_hsl_x = 310;
	cfg->label_positions.label_hsl_y = 75;
	cfg->label_positions.label_hsl_width = 60;
	cfg->label_positions.label_hsl_padding = 4;
	cfg->label_positions.label_hsl_border_width = 1;
	cfg->label_positions.label_hsl_border_radius = 0;
	cfg->label_positions.label_hsl_border_enabled = 0;
	cfg->label_positions.label_rgbf_x = 310;
	cfg->label_positions.label_rgbf_y = 110;
	cfg->label_positions.label_rgbf_width = 60;
	cfg->label_positions.label_rgbf_padding = 4;
	cfg->label_positions.label_rgbf_border_width = 1;
	cfg->label_positions.label_rgbf_border_radius = 0;
	cfg->label_positions.label_rgbf_border_enabled = 0;
	cfg->label_positions.label_rgbi_x = 310;
	cfg->label_positions.label_rgbi_y = 145;
	cfg->label_positions.label_rgbi_width = 60;
	cfg->label_positions.label_rgbi_padding = 4;
	cfg->label_positions.label_rgbi_border_width = 1;
	cfg->label_positions.label_rgbi_border_radius = 0;
	cfg->label_positions.label_rgbi_border_enabled = 0;
	cfg->label_positions.label_hex_x = 310;
	cfg->label_positions.label_hex_y = 180;
	cfg->label_positions.label_hex_width = 60;
	cfg->label_positions.label_hex_padding = 4;
	cfg->label_positions.label_hex_border_width = 1;
	cfg->label_positions.label_hex_border_radius = 0;
	cfg->label_positions.label_hex_border_enabled = 0;

// Window dimensions now in main struct
	cfg->main.main_width = 590;
	cfg->main.main_height = 300;
	cfg->main.about_width = 590;
	cfg->main.about_height = 300;

// Selection defaults
	cfg->selection_color = (ConfigColor) {
		0.26, 0.51, 0.96, 1.0
	};
	cfg->selection_text_color = (ConfigColor) {
		1.0, 1.0, 1.0, 1.0
	};
	cfg->undo_depth = 64;

// Max lengths
	cfg->max_length.text = 256;
	cfg->max_length.integer = 12;
	cfg->max_length.floating = 32;
	cfg->max_length.hex = 7;

// Menu items
	strncpy(cfg->menu_items[0], "Cut", 31);
	strncpy(cfg->menu_items[1], "Copy", 31);
	strncpy(cfg->menu_items[2], "Paste", 31);
	strncpy(cfg->menu_items[3], "Undo", 31);
	strncpy(cfg->menu_items[4], "Redo", 31);
	cfg->menu_item_count = 5;

// Paths - use smart detection
	detect_best_editor(cfg->editor_path, 255);
	detect_best_browser(cfg->browser_path, 255);

// Window Management defaults
//	cfg->remember_position = 1;
//	cfg->always_on_top = 0;
//	cfg->minimize_to_tray = 0;
//	cfg->window_x = 0; // User's preferred position
//	cfg->window_y = 26; // User's preferred position

// Duplicate sections removed - values already set above
}

/* ========== CONFIGURATION FILE WRITING ========== */

int config_write_defaults(const char *path) {
	PixelPrismConfig cfg;
	config_init_defaults(&cfg);

	FILE *f = fopen(path, "w");
	if (!f) {
		return -1; // Failed to open file
	}
	int result = config_write_defaults_with_values(f, &cfg);
	fclose(f);
	return result ? 0 : -1;
}

/*
 * Emit a configuration file using the current values inside cfg. Only the
 * non-derived sections are written; widget sections are delegated through
 * the registry to keep the ordering consistent with registration.
 */
int config_write_defaults_with_values(FILE *f, PixelPrismConfig *cfg) {
	if (!f || !cfg) {
		return 0;
	}
	
	// Header - Styling Section
	fprintf(f, "# ============================================================================\n");
	fprintf(f, "# VISUAL STYLING\n");
	fprintf(f, "# All color, font, and appearance settings for UI elements.\n");
	fprintf(f, "# Sections and keys within each section are alphabetically ordered.\n");
	fprintf(f, "# ============================================================================\n\n");

	// Write all styling sections via registry (button, entries, label, menu, menubar, swatch, tray-menu, zoom)
	struct ConfigWriteContext ctx = {
		.f = f,
		.cfg = cfg,
	};
	config_registry_for_each(registry_write_cb, &ctx);

	// Separator - Configuration Section
	fprintf(f, "# ============================================================================\n");
	fprintf(f, "# CONFIGURATION & BEHAVIOR\n");
	fprintf(f, "# Widget geometry, positioning, application behavior, and system settings.\n");
	fprintf(f, "# Sections and keys within each section are alphabetically ordered.\n");
	fprintf(f, "# ============================================================================\n\n");

	// ========== [behavior] ==========
	fprintf(f, "[behavior]\n");
	fprintf(f, "always-on-top = %s\n", cfg->always_on_top ? "true" : "false");
	fprintf(f, "cursor-blink-ms = %d\n", cfg->cursor_blink_ms);
	fprintf(f, "cursor-color = #%02X%02X%02X\n", (int)(cfg->cursor_color.r * 255), (int)(cfg->cursor_color.g * 255), (int)(cfg->cursor_color.b * 255));
	fprintf(f, "cursor-width = %d\n", cfg->cursor_thickness);
	fprintf(f, "hex-case = %s\n", cfg->hex_uppercase ? "upper" : "lower");
	fprintf(f, "minimize-to-tray = %s\n", cfg->minimize_to_tray ? "true" : "false");
	fprintf(f, "remember-position = %s\n", cfg->remember_position ? "true" : "false");
	fprintf(f, "show-tray-icon = %s\n", cfg->show_tray_icon ? "true" : "false");
	fprintf(f, "undo-depth = %d\n\n", cfg->undo_depth);

	// ========== [button-widget] ==========
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

	// ========== [clipboard] ==========
	fprintf(f, "[clipboard]\n");
	fprintf(f, "auto-copy = %s\n", cfg->auto_copy ? "true" : "false");
	fprintf(f, "# Options: hex, hsv, hsl, rgb, rgbi\n");
	fprintf(f, "auto-copy-format = %s\n", cfg->auto_copy_format);
	fprintf(f, "auto-copy-primary = %s\n", cfg->auto_copy_primary ? "true" : "false");
	fprintf(f, "hex-prefix = %s\n\n", cfg->hex_prefix ? "true" : "false");

	// ========== Entry widget instances (alphabetical: hex, hsl, hsv, rgbf, rgbi) ==========
	fprintf(f, "[entry-widget-hex]\nborder-radius = %d\nborder-width = %d\nentry-hex-x = %d\nentry-hex-y = %d\npadding = %d\nwidth = %d\n\n", cfg->entry_positions.entry_hex_border_radius, cfg->entry_positions.entry_hex_border_width, cfg->entry_positions.entry_hex_x, cfg->entry_positions.entry_hex_y, cfg->entry_positions.entry_hex_padding, cfg->entry_positions.entry_hex_width);
	fprintf(f, "[entry-widget-hsl]\nborder-radius = %d\nborder-width = %d\nentry-hsl-x = %d\nentry-hsl-y = %d\npadding = %d\nwidth = %d\n\n", cfg->entry_positions.entry_hsl_border_radius, cfg->entry_positions.entry_hsl_border_width, cfg->entry_positions.entry_hsl_x, cfg->entry_positions.entry_hsl_y, cfg->entry_positions.entry_hsl_padding, cfg->entry_positions.entry_hsl_width);
	fprintf(f, "[entry-widget-hsv]\nborder-radius = %d\nborder-width = %d\nentry-hsv-x = %d\nentry-hsv-y = %d\npadding = %d\nwidth = %d\n\n", cfg->entry_positions.entry_hsv_border_radius, cfg->entry_positions.entry_hsv_border_width, cfg->entry_positions.entry_hsv_x, cfg->entry_positions.entry_hsv_y, cfg->entry_positions.entry_hsv_padding, cfg->entry_positions.entry_hsv_width);
	fprintf(f, "[entry-widget-rgbf]\nborder-radius = %d\nborder-width = %d\nentry-rgbf-x = %d\nentry-rgbf-y = %d\npadding = %d\nwidth = %d\n\n", cfg->entry_positions.entry_rgbf_border_radius, cfg->entry_positions.entry_rgbf_border_width, cfg->entry_positions.entry_rgbf_x, cfg->entry_positions.entry_rgbf_y, cfg->entry_positions.entry_rgbf_padding, cfg->entry_positions.entry_rgbf_width);
	fprintf(f, "[entry-widget-rgbi]\nborder-radius = %d\nborder-width = %d\nentry-rgbi-x = %d\nentry-rgbi-y = %d\npadding = %d\nwidth = %d\n\n", cfg->entry_positions.entry_rgbi_border_radius, cfg->entry_positions.entry_rgbi_border_width, cfg->entry_positions.entry_rgbi_x, cfg->entry_positions.entry_rgbi_y, cfg->entry_positions.entry_rgbi_padding, cfg->entry_positions.entry_rgbi_width);

	// ========== Label widget instances (alphabetical: hex, hsl, hsv, rgbf, rgbi) ==========
	fprintf(f, "[label-widget-hex]\nborder-enabled = %s\nborder-radius = %d\nborder-width = %d\nlabel-hex-x = %d\nlabel-hex-y = %d\npadding = %d\nwidth = %d\n\n", cfg->label_positions.label_hex_border_enabled ? "true" : "false", cfg->label_positions.label_hex_border_radius, cfg->label_positions.label_hex_border_width, cfg->label_positions.label_hex_x, cfg->label_positions.label_hex_y, cfg->label_positions.label_hex_padding, cfg->label_positions.label_hex_width);
	fprintf(f, "[label-widget-hsl]\nborder-enabled = %s\nborder-radius = %d\nborder-width = %d\nlabel-hsl-x = %d\nlabel-hsl-y = %d\npadding = %d\nwidth = %d\n\n", cfg->label_positions.label_hsl_border_enabled ? "true" : "false", cfg->label_positions.label_hsl_border_radius, cfg->label_positions.label_hsl_border_width, cfg->label_positions.label_hsl_x, cfg->label_positions.label_hsl_y, cfg->label_positions.label_hsl_padding, cfg->label_positions.label_hsl_width);
	fprintf(f, "[label-widget-hsv]\nborder-enabled = %s\nborder-radius = %d\nborder-width = %d\nlabel-hsv-x = %d\nlabel-hsv-y = %d\npadding = %d\nwidth = %d\n\n", cfg->label_positions.label_hsv_border_enabled ? "true" : "false", cfg->label_positions.label_hsv_border_radius, cfg->label_positions.label_hsv_border_width, cfg->label_positions.label_hsv_x, cfg->label_positions.label_hsv_y, cfg->label_positions.label_hsv_padding, cfg->label_positions.label_hsv_width);
	fprintf(f, "[label-widget-rgbf]\nborder-enabled = %s\nborder-radius = %d\nborder-width = %d\nlabel-rgbf-x = %d\nlabel-rgbf-y = %d\npadding = %d\nwidth = %d\n\n", cfg->label_positions.label_rgbf_border_enabled ? "true" : "false", cfg->label_positions.label_rgbf_border_radius, cfg->label_positions.label_rgbf_border_width, cfg->label_positions.label_rgbf_x, cfg->label_positions.label_rgbf_y, cfg->label_positions.label_rgbf_padding, cfg->label_positions.label_rgbf_width);
	fprintf(f, "[label-widget-rgbi]\nborder-enabled = %s\nborder-radius = %d\nborder-width = %d\nlabel-rgbi-x = %d\nlabel-rgbi-y = %d\npadding = %d\nwidth = %d\n\n", cfg->label_positions.label_rgbi_border_enabled ? "true" : "false", cfg->label_positions.label_rgbi_border_radius, cfg->label_positions.label_rgbi_border_width, cfg->label_positions.label_rgbi_x, cfg->label_positions.label_rgbi_y, cfg->label_positions.label_rgbi_padding, cfg->label_positions.label_rgbi_width);

	// ========== [main] ==========
	fprintf(f, "[main]\n");
	fprintf(f, "about-height = %d\n", cfg->main.about_height);
	fprintf(f, "about-width = %d\n", cfg->main.about_width);
	fprintf(f, "background = #%02X%02X%02X\n", (int)(cfg->main.background.r * 255), (int)(cfg->main.background.g * 255), (int)(cfg->main.background.b * 255));
	fprintf(f, "color = #%02X%02X%02X\n", (int)(cfg->main.text_color.r * 255), (int)(cfg->main.text_color.g * 255), (int)(cfg->main.text_color.b * 255));
	fprintf(f, "font = %s\n", cfg->main.font_family);
	fprintf(f, "font-size = %d\n", cfg->main.font_size);
	fprintf(f, "link-color = #%02X%02X%02X\n", (int)(cfg->main.link_color.r * 255), (int)(cfg->main.link_color.g * 255), (int)(cfg->main.link_color.b * 255));
	fprintf(f, "link-underline = %s\n", cfg->main.link_underline ? "true" : "false");
	fprintf(f, "main-height = %d\n", cfg->main.main_height);
	fprintf(f, "main-width = %d\n\n", cfg->main.main_width);

	// ========== [menubar-widget] ==========
	fprintf(f, "[menubar-widget]\n");
	fprintf(f, "border-radius = %d\n", cfg->menubar_widget.border_radius);
	fprintf(f, "border-width = %d\n", cfg->menubar_widget.border_width);
	fprintf(f, "menubar-x = %d\n", cfg->menubar_widget.menubar_x);
	fprintf(f, "menubar-y = %d\n", cfg->menubar_widget.menubar_y);
	fprintf(f, "padding = %d\n", cfg->menubar_widget.padding);
	fprintf(f, "width = %d\n\n", cfg->menubar_widget.width);

	// ========== [paths] ==========
	fprintf(f, "[paths]\n");
	fprintf(f, "browser = %s\n", cfg->browser_path);
	fprintf(f, "editor = %s\n\n", cfg->editor_path);

	// ========== [swatch-widget] ==========
	fprintf(f, "[swatch-widget]\n");
	fprintf(f, "border-mode = ");
	switch (cfg->swatch_border_mode) {
		case BORDER_MODE_COMPLEMENTARY: fprintf(f, "complementary\n"); break;
		case BORDER_MODE_CONTRAST: fprintf(f, "contrast\n"); break;
		case BORDER_MODE_TRIADIC: fprintf(f, "triadic\n"); break;
		default: fprintf(f, "complementary\n"); break;
	}
	fprintf(f, "border-radius = %d\n", cfg->swatch_widget.border_radius);
	fprintf(f, "border-width = %d\n", cfg->swatch_widget.border_width);
	fprintf(f, "height = %d\n", cfg->swatch_widget.height);
	fprintf(f, "swatch-x = %d\n", cfg->swatch_widget.swatch_x);
	fprintf(f, "swatch-y = %d\n", cfg->swatch_widget.swatch_y);
	fprintf(f, "width = %d\n\n", cfg->swatch_widget.width);

	// ========== [tray-menu-widget] ==========
	fprintf(f, "[tray-menu-widget]\n");
	fprintf(f, "border-radius = %d\n", cfg->tray_menu_widget.border_radius);
	fprintf(f, "border-width = %d\n", cfg->tray_menu_widget.border_width);
	fprintf(f, "padding = %d\n\n", cfg->tray_menu_widget.padding);

	// ========== [zoom-widget] ==========
	fprintf(f, "[zoom-widget]\n");
	fprintf(f, "crosshair-show = %s\n", cfg->zoom_widget.crosshair_show ? "true" : "false");
	fprintf(f, "crosshair-show-after-pick = %s\n", cfg->zoom_widget.crosshair_show_after_pick ? "true" : "false");
	fprintf(f, "square-show = %s\n", cfg->zoom_widget.square_show ? "true" : "false");
	fprintf(f, "square-show-after-pick = %s\n\n", cfg->zoom_widget.square_show_after_pick ? "true" : "false");

	return 1;
}

/* ========== CONFIGURATION FILE PARSING ========== */

/* --- Parsing Helper Functions --- */

/* Parse boolean value from string */
static int parse_bool(const char *value) {
	return (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
}

/* Parse label widget position section - reduces duplication */
static void parse_label_widget_section(const char *section, const char *key, const char *value, PixelPrismConfig *cfg) {
	int *border_enabled = NULL, *border_radius = NULL, *border_width = NULL;
	int *x = NULL, *y = NULL, *padding = NULL, *width = NULL;
	
	if (strcmp(section, "label-widget-hsv") == 0) {
		border_enabled = &cfg->label_positions.label_hsv_border_enabled;
		border_radius = &cfg->label_positions.label_hsv_border_radius;
		border_width = &cfg->label_positions.label_hsv_border_width;
		x = &cfg->label_positions.label_hsv_x;
		y = &cfg->label_positions.label_hsv_y;
		padding = &cfg->label_positions.label_hsv_padding;
		width = &cfg->label_positions.label_hsv_width;
	} else if (strcmp(section, "label-widget-hsl") == 0) {
		border_enabled = &cfg->label_positions.label_hsl_border_enabled;
		border_radius = &cfg->label_positions.label_hsl_border_radius;
		border_width = &cfg->label_positions.label_hsl_border_width;
		x = &cfg->label_positions.label_hsl_x;
		y = &cfg->label_positions.label_hsl_y;
		padding = &cfg->label_positions.label_hsl_padding;
		width = &cfg->label_positions.label_hsl_width;
	} else if (strcmp(section, "label-widget-rgbf") == 0) {
		border_enabled = &cfg->label_positions.label_rgbf_border_enabled;
		border_radius = &cfg->label_positions.label_rgbf_border_radius;
		border_width = &cfg->label_positions.label_rgbf_border_width;
		x = &cfg->label_positions.label_rgbf_x;
		y = &cfg->label_positions.label_rgbf_y;
		padding = &cfg->label_positions.label_rgbf_padding;
		width = &cfg->label_positions.label_rgbf_width;
	} else if (strcmp(section, "label-widget-rgbi") == 0) {
		border_enabled = &cfg->label_positions.label_rgbi_border_enabled;
		border_radius = &cfg->label_positions.label_rgbi_border_radius;
		border_width = &cfg->label_positions.label_rgbi_border_width;
		x = &cfg->label_positions.label_rgbi_x;
		y = &cfg->label_positions.label_rgbi_y;
		padding = &cfg->label_positions.label_rgbi_padding;
		width = &cfg->label_positions.label_rgbi_width;
	} else if (strcmp(section, "label-widget-hex") == 0) {
		border_enabled = &cfg->label_positions.label_hex_border_enabled;
		border_radius = &cfg->label_positions.label_hex_border_radius;
		border_width = &cfg->label_positions.label_hex_border_width;
		x = &cfg->label_positions.label_hex_x;
		y = &cfg->label_positions.label_hex_y;
		padding = &cfg->label_positions.label_hex_padding;
		width = &cfg->label_positions.label_hex_width;
	} else {
		return;
	}
	
	// Parse common fields
	if (strcmp(key, "border-enabled") == 0) *border_enabled = parse_bool(value);
	else if (strcmp(key, "border-radius") == 0) *border_radius = atoi(value);
	else if (strcmp(key, "border-width") == 0) *border_width = atoi(value);
	else if (strstr(key, "-x")) *x = atoi(value);
	else if (strstr(key, "-y")) *y = atoi(value);
	else if (strcmp(key, "padding") == 0) *padding = atoi(value);
	else if (strcmp(key, "width") == 0) *width = atoi(value);
}

/* Parse entry widget position section - reduces duplication */
static void parse_entry_widget_section(const char *section, const char *key, const char *value, PixelPrismConfig *cfg) {
	int *border_radius = NULL, *border_width = NULL;
	int *x = NULL, *y = NULL, *padding = NULL, *width = NULL;
	
	if (strcmp(section, "entry-widget-hsv") == 0) {
		border_radius = &cfg->entry_positions.entry_hsv_border_radius;
		border_width = &cfg->entry_positions.entry_hsv_border_width;
		x = &cfg->entry_positions.entry_hsv_x;
		y = &cfg->entry_positions.entry_hsv_y;
		padding = &cfg->entry_positions.entry_hsv_padding;
		width = &cfg->entry_positions.entry_hsv_width;
	} else if (strcmp(section, "entry-widget-hsl") == 0) {
		border_radius = &cfg->entry_positions.entry_hsl_border_radius;
		border_width = &cfg->entry_positions.entry_hsl_border_width;
		x = &cfg->entry_positions.entry_hsl_x;
		y = &cfg->entry_positions.entry_hsl_y;
		padding = &cfg->entry_positions.entry_hsl_padding;
		width = &cfg->entry_positions.entry_hsl_width;
	} else if (strcmp(section, "entry-widget-rgbf") == 0) {
		border_radius = &cfg->entry_positions.entry_rgbf_border_radius;
		border_width = &cfg->entry_positions.entry_rgbf_border_width;
		x = &cfg->entry_positions.entry_rgbf_x;
		y = &cfg->entry_positions.entry_rgbf_y;
		padding = &cfg->entry_positions.entry_rgbf_padding;
		width = &cfg->entry_positions.entry_rgbf_width;
	} else if (strcmp(section, "entry-widget-rgbi") == 0) {
		border_radius = &cfg->entry_positions.entry_rgbi_border_radius;
		border_width = &cfg->entry_positions.entry_rgbi_border_width;
		x = &cfg->entry_positions.entry_rgbi_x;
		y = &cfg->entry_positions.entry_rgbi_y;
		padding = &cfg->entry_positions.entry_rgbi_padding;
		width = &cfg->entry_positions.entry_rgbi_width;
	} else if (strcmp(section, "entry-widget-hex") == 0) {
		border_radius = &cfg->entry_positions.entry_hex_border_radius;
		border_width = &cfg->entry_positions.entry_hex_border_width;
		x = &cfg->entry_positions.entry_hex_x;
		y = &cfg->entry_positions.entry_hex_y;
		padding = &cfg->entry_positions.entry_hex_padding;
		width = &cfg->entry_positions.entry_hex_width;
	} else {
		return;
	}
	
	// Parse common fields
	if (strcmp(key, "border-radius") == 0) *border_radius = atoi(value);
	else if (strcmp(key, "border-width") == 0) *border_width = atoi(value);
	else if (strstr(key, "-x")) *x = atoi(value);
	else if (strstr(key, "-y")) *y = atoi(value);
	else if (strcmp(key, "padding") == 0) *padding = atoi(value);
	else if (strcmp(key, "width") == 0) *width = atoi(value);
}

/* Load configuration from file */
/*
 * Parse a config file from disk into cfg. Defaults are applied first, then
 * each section is fed to the appropriate registry handler or legacy block.
 */
int config_load(PixelPrismConfig *cfg, const char *path) {
	if (!cfg || !path) {
		return -1;
	}
	// Initialize with defaults first
	config_init_defaults(cfg);

	FILE *f = fopen(path, "r");
	if (!f) {
		// Config file doesn't exist, use defaults
		return 0;
	}
	char line[512];
	char section[64] = "";
	while (fgets(line, sizeof(line), f)) {
		// Trim leading whitespace first
		char *ptr = line;
		while (*ptr && isspace((unsigned char)*ptr)) {
			ptr++;
		}
		// Skip comment lines (lines starting with #)
		if (*ptr == '#' || *ptr == '\0') {
			continue;
		}
		// Trim trailing whitespace
		char *end = ptr + strlen(ptr) - 1;
		while (end > ptr && isspace((unsigned char)*end)) {
			end--;
		}
		*(end + 1) = '\0';
		// Check for section header
		if (*ptr == '[' && *end == ']') {
			size_t len = (size_t)(end - ptr - 1);
			if (len < sizeof(section)) {
				memcpy(section, ptr + 1, len);
				section[len] = '\0';
			}
			continue;
		}
		// Parse key=value
		char *eq = strchr(ptr, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		char *key = ptr;
		char *value = eq + 1;
		// Trim key and value
		while (*key && isspace((unsigned char)*key)) {
			key++;
		}
		end = key + strlen(key) - 1;
		while (end > key && isspace((unsigned char)*end)) {
			end--;
		}
		*(end + 1) = '\0';
		while (*value && isspace((unsigned char)*value)) {
			value++;
		}
		end = value + strlen(value) - 1;
		while (end > value && isspace((unsigned char)*end)) {
			end--;
		}
		*(end + 1) = '\0';
		// Parse based on section
		const ConfigSectionHandler *section_handler = config_registry_find(section);
		if (section_handler && section_handler->parse && section_handler->parse(cfg, key, value)) {
			continue;
		}
		// Label widget position sections - use helper function
		if (strncmp(section, "label-widget-", 13) == 0) {
			parse_label_widget_section(section, key, value, cfg);
		}
		// Entry widget position sections - use helper function
		else if (strncmp(section, "entry-widget-", 13) == 0) {
			parse_entry_widget_section(section, key, value, cfg);
		}
		else if (strcmp(section, "entry-text") == 0) {
			if (strcmp(key, "font") == 0 || strcmp(key, "font-family") == 0) {
				strncpy(cfg->entry_text.font_family, value, sizeof(cfg->entry_text.font_family) - 1);
			}
			else if (strcmp(key, "font-size") == 0) {
				cfg->entry_text.font_size = atoi(value);
			}
			else if (strcmp(key, "color") == 0) {
				cfg->entry_text.fg = parse_color(value);
			}
			else if (strcmp(key, "background") == 0) {
				cfg->entry_text.bg = parse_color(value);
			}
			else if (strcmp(key, "border") == 0) {
				cfg->entry_text.border = parse_color(value);
			}
			else if (strcmp(key, "valid-border") == 0) {
				cfg->entry_text.valid_border = parse_color(value);
			}
			else if (strcmp(key, "invalid-border") == 0) {
				cfg->entry_text.invalid_border = parse_color(value);
			}
			else if (strcmp(key, "active-border") == 0) {
				cfg->entry_text.invalid_border = parse_color(value);
			}
			else if (strcmp(key, "hover-border") == 0) {
				cfg->entry_text.valid_border = parse_color(value);
			}
		}
		else if (strcmp(section, "entry-int") == 0) {
			if (strcmp(key, "font") == 0 || strcmp(key, "font-family") == 0) {
				strncpy(cfg->entry_int.font_family, value, sizeof(cfg->entry_int.font_family) - 1);
			}
			else if (strcmp(key, "font-size") == 0) {
				cfg->entry_int.font_size = atoi(value);
			}
			else if (strcmp(key, "color") == 0) {
				cfg->entry_int.fg = parse_color(value);
			}
			else if (strcmp(key, "background") == 0) {
				cfg->entry_int.bg = parse_color(value);
			}
			else if (strcmp(key, "border") == 0) {
				cfg->entry_int.border = parse_color(value);
			}
			else if (strcmp(key, "valid-border") == 0) {
				cfg->entry_int.valid_border = parse_color(value);
			}
			else if (strcmp(key, "invalid-border") == 0) {
				cfg->entry_int.invalid_border = parse_color(value);
			}
		}
		else if (strcmp(section, "entry-float") == 0) {
			if (strcmp(key, "font") == 0 || strcmp(key, "font-family") == 0) {
				strncpy(cfg->entry_float.font_family, value, sizeof(cfg->entry_float.font_family) - 1);
			}
			else if (strcmp(key, "font-size") == 0) {
				cfg->entry_float.font_size = atoi(value);
			}
			else if (strcmp(key, "color") == 0) {
				cfg->entry_float.fg = parse_color(value);
			}
			else if (strcmp(key, "background") == 0) {
				cfg->entry_float.bg = parse_color(value);
			}
			else if (strcmp(key, "border") == 0) {
				cfg->entry_float.border = parse_color(value);
			}
			else if (strcmp(key, "valid-border") == 0) {
				cfg->entry_float.valid_border = parse_color(value);
			}
			else if (strcmp(key, "invalid-border") == 0) {
				cfg->entry_float.invalid_border = parse_color(value);
			}
			else if (strcmp(key, "active-border") == 0) {
				cfg->entry_float.invalid_border = parse_color(value);
			}
			else if (strcmp(key, "hover-border") == 0) {
				cfg->entry_float.valid_border = parse_color(value);
			}
		}
		else if (strcmp(section, "entry-hex") == 0) {
			if (strcmp(key, "font") == 0 || strcmp(key, "font-family") == 0) {
				strncpy(cfg->entry_hex.font_family, value, sizeof(cfg->entry_hex.font_family) - 1);
			}
			else if (strcmp(key, "font-size") == 0) {
				cfg->entry_hex.font_size = atoi(value);
			}
			else if (strcmp(key, "color") == 0) {
				cfg->entry_hex.fg = parse_color(value);
			}
			else if (strcmp(key, "background") == 0) {
				cfg->entry_hex.bg = parse_color(value);
			}
			else if (strcmp(key, "border") == 0) {
				cfg->entry_hex.border = parse_color(value);
			}
			else if (strcmp(key, "valid-border") == 0) {
				cfg->entry_hex.valid_border = parse_color(value);
			}
			else if (strcmp(key, "invalid-border") == 0) {
				cfg->entry_hex.invalid_border = parse_color(value);
			}
		}
		else if (strcmp(section, "button") == 0) {
			// Delegated to button widget
			button_config_parse(&cfg->button, key, value);
		}
		else if (strcmp(section, "label") == 0) {
			// Delegated to label widget
			label_config_parse((LabelConfig*)&cfg->label, key, value);
		}
		else if (strcmp(section, "menu") == 0) {
			if (strcmp(key, "font") == 0 || strcmp(key, "font-family") == 0) {
				strncpy(cfg->menu.font_family, value, sizeof(cfg->menu.font_family) - 1);
			}
			else if (strcmp(key, "font-size") == 0) {
				cfg->menu.font_size = atoi(value);
			}
			else if (strcmp(key, "color") == 0) {
				cfg->menu.fg = parse_color(value);
			}
			else if (strcmp(key, "background") == 0) {
			}
			else if (strcmp(key, "active-background") == 0) {
				cfg->menu.active_bg = parse_color(value);
			}
			else if (strcmp(key, "hover-background") == 0) {
				cfg->menu.hover_bg = parse_color(value);
			}
		}
		else if (strcmp(section, "main") == 0) {
			if (strcmp(key, "about-height") == 0) {
				cfg->main.about_height = atoi(value);
			}
			else if (strcmp(key, "about-width") == 0) {
				cfg->main.about_width = atoi(value);
			}
			else if (strcmp(key, "background") == 0) {
				cfg->main.background = parse_color(value);
			}
			else if (strcmp(key, "color") == 0) {
				cfg->main.text_color = parse_color(value);
			}
			else if (strcmp(key, "font") == 0 || strcmp(key, "font-family") == 0) {
				strncpy(cfg->main.font_family, value, sizeof(cfg->main.font_family) - 1);
			}
			else if (strcmp(key, "font-size") == 0) {
				cfg->main.font_size = atoi(value);
			}
			else if (strcmp(key, "link-color") == 0) {
				cfg->main.link_color = parse_color(value);
			}
			else if (strcmp(key, "link-underline") == 0) {
				cfg->main.link_underline = parse_bool(value);
			}
			else if (strcmp(key, "main-height") == 0) {
				cfg->main.main_height = atoi(value);
			}
			else if (strcmp(key, "main-width") == 0) {
				cfg->main.main_width = atoi(value);
			}
			else if (strcmp(key, "current-color") == 0) {
				cfg->current_color = parse_color(value);
			}
		}
		else if (strcmp(section, "menubar") == 0) {
			if (strcmp(key, "font") == 0 || strcmp(key, "font-family") == 0) {
				strncpy(cfg->menubar.font_family, value, sizeof(cfg->menubar.font_family) - 1);
			}
			else if (strcmp(key, "font-size") == 0) {
				cfg->menubar.font_size = atoi(value);
			}
			else if (strcmp(key, "color") == 0) {
				cfg->menubar.fg = parse_color(value);
			}
			else if (strcmp(key, "background") == 0) {
				cfg->menubar.bg = parse_color(value);
			}
			else if (strcmp(key, "active-background") == 0) {
				cfg->menubar.active_bg = parse_color(value);
			}
			else if (strcmp(key, "hover-background") == 0) {
				cfg->menubar.hover_bg = parse_color(value);
			}
			else if (strcmp(key, "border") == 0) {
				cfg->menubar.border = parse_color(value);
			}
		}
		else if (strcmp(section, "menubar-widget") == 0) {
			menubar_widget_section_parse(cfg, key, value);
		}
		else if (strcmp(section, "paths") == 0) {
			if (strcmp(key, "browser") == 0) {
				strncpy(cfg->browser_path, value, sizeof(cfg->browser_path) - 1);
			}
			else if (strcmp(key, "editor") == 0) {
				strncpy(cfg->editor_path, value, sizeof(cfg->editor_path) - 1);
			}
		}
		else if (strcmp(section, "swatch") == 0) {
			// Delegate to swatch widget
			swatch_config_parse(cfg, key, value);
			
			// Handle border-mode (behavior setting, not widget config)
			if (strcmp(key, "border-mode") == 0) {
				if (strcmp(value, "complementary") == 0) {
					cfg->swatch_border_mode = BORDER_MODE_COMPLEMENTARY;
				}
				else if (strcmp(value, "inverse") == 0) {
					cfg->swatch_border_mode = BORDER_MODE_INVERSE;
				}
				else if (strcmp(value, "contrast") == 0) {
					cfg->swatch_border_mode = BORDER_MODE_CONTRAST;
				}
				else if (strcmp(value, "triadic") == 0) {
					cfg->swatch_border_mode = BORDER_MODE_TRIADIC;
				}
				current_border_mode = cfg->swatch_border_mode;
			}
		}
		else if (strcmp(section, "swatch-widget") == 0) {
			swatch_widget_section_parse(cfg, key, value);
		}
		else if (strcmp(section, "zoom-widget") == 0) {
			zoom_widget_section_parse(cfg, key, value);
		}
		else if (strcmp(section, "behavior") == 0) {
			if (strcmp(key, "always-on-top") == 0) {
				cfg->always_on_top = parse_bool(value);
			}
			else if (strcmp(key, "cursor-blink-ms") == 0) {
				cfg->cursor_blink_ms = atoi(value);
			}
			else if (strcmp(key, "cursor-color") == 0) {
				cfg->cursor_color = parse_color(value);
			}
			else if (strcmp(key, "cursor-width") == 0) {
				cfg->cursor_thickness = atoi(value);
			}
			else if (strcmp(key, "hex-case") == 0) {
				cfg->hex_uppercase = (strcmp(value, "upper") == 0 || strcmp(value, "1") == 0);
			}
			else if (strcmp(key, "minimize-to-tray") == 0) {
				cfg->minimize_to_tray = parse_bool(value);
			}
			else if (strcmp(key, "remember-position") == 0) {
				cfg->remember_position = parse_bool(value);
			}
			else if (strcmp(key, "show-tray-icon") == 0) {
				cfg->show_tray_icon = parse_bool(value);
			}
			else if (strcmp(key, "undo-depth") == 0) {
				cfg->undo_depth = atoi(value);
			}
		}
		else if (strcmp(section, "button-widget") == 0) {
			button_widget_section_parse(cfg, key, value);
		}
		else if (strcmp(section, "clipboard") == 0) {
			if (strcmp(key, "auto-copy") == 0) {
				cfg->auto_copy = parse_bool(value);
			}
			else if (strcmp(key, "auto-copy-format") == 0) {
				strncpy(cfg->auto_copy_format, value, sizeof(cfg->auto_copy_format) - 1);
			}
			else if (strcmp(key, "auto-copy-primary") == 0) {
				cfg->auto_copy_primary = parse_bool(value);
			}
			else if (strcmp(key, "hex-prefix") == 0) {
				cfg->hex_prefix = parse_bool(value);
			}
		}
		else if (strcmp(section, "tray-menu") == 0) {
			if (strcmp(key, "font") == 0 || strcmp(key, "font-family") == 0) {
				strncpy(cfg->tray_menu.font_family, value, sizeof(cfg->tray_menu.font_family) - 1);
			}
			else if (strcmp(key, "font-size") == 0) {
				cfg->tray_menu.font_size = atoi(value);
			}
			else if (strcmp(key, "color") == 0 || strcmp(key, "fg") == 0) {
				cfg->tray_menu.fg = parse_color(value);
			}
			else if (strcmp(key, "background") == 0 || strcmp(key, "bg") == 0) {
				cfg->tray_menu.bg = parse_color(value);
			}
			else if (strcmp(key, "hover-background") == 0) {
				cfg->tray_menu.hover_bg = parse_color(value);
			}
			else if (strcmp(key, "hover-bg") == 0) {
				// Backward compatibility
				cfg->tray_menu.hover_bg = parse_color(value);
			}
			else if (strcmp(key, "border") == 0) {
				cfg->tray_menu.border = parse_color(value);
			}
		}
		else if (strcmp(section, "tray-menu-widget") == 0) {
			if (strcmp(key, "border-radius") == 0) {
				cfg->tray_menu_widget.border_radius = atoi(value);
			}
			else if (strcmp(key, "border-width") == 0) {
				cfg->tray_menu_widget.border_width = atoi(value);
			}
			else if (strcmp(key, "padding") == 0) {
				cfg->tray_menu_widget.padding = atoi(value);
			}
		}
	}
	fclose(f);
	return 0; // Success
}

/* Initialize configuration system */
void config_init(PixelPrismConfig *cfg) {
	config_init_defaults(cfg);
}

/* Mark configuration as changed */
void config_mark_changed(PixelPrismConfig *cfg) {
	if (cfg) {
		cfg->config_changed = 1;
	}
}

/* Mark configuration as saved */
void config_mark_saved(PixelPrismConfig *cfg) {
	if (cfg) {
		cfg->config_changed = 0;
	}
}

/* Check if configuration has unsaved changes */
int config_has_unsaved_changes(PixelPrismConfig *cfg) {
	return cfg ? cfg->config_changed : 0;
}

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

	// Register signal handlers for proper cleanup
	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);

	// Register cleanup function to be called on exit
	atexit(cleanup_all_widgets);

	pixelprism();
	return 0;
}
