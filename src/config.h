#ifndef CONFIG_H_
#define CONFIG_H_

/* ========== CONFIGURATION SYSTEM INTERFACE ========== */

/**
 * @file config.h
 * @brief Configuration and theming system for the widget toolkit
 *
 * Self-contained CONF-style configuration parser and theme management system.
 * Handles reading/writing configuration files, color theme definitions, and
 * widget styling without external dependencies.
 *
 * Features:
 * - CONF format parser (key=value pairs)
 * - Color theme management (RGB, borders, fonts)
 * - Widget-specific styling blocks
 * - Automatic config file watching and reloading
 * - Default fallback themes
 * - Dirty state tracking for save prompts
 *
 * Usage:
 *   1. Load config: config = load_config("path/to/config.conf")
 *   2. Access values: config.base_theme.bg, config.button_block.label, etc.
 *   3. Modify values: config.base_theme.bg = (RGBA){0.1, 0.1, 0.1, 1.0}
 *   4. Save config: save_config("path/to/config.conf", &config)
 *   5. Free resources: free_config(&config)
 *
 * Configuration files use simple key=value format with sections.
 * Colors can be specified as hex (#RRGGBB) or float RGBA values.
 *
 * Dependencies:
 * - None (pure C with standard library)
 *
 * This module is completely standalone and can be used independently
 * for configuration management in any C application.
 */

#include <stdio.h>
#include <stddef.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

/* ========== TYPE DEFINITIONS ========== */

/* Color structure - RGBA float values [0.0 - 1.0] */
typedef struct {
	double r;
	double g;
	double b;
	double a;
} ConfigColor;

/* Swatch border mode enum */
typedef enum {
	BORDER_MODE_COMPLEMENTARY = 0,
	BORDER_MODE_INVERSE,
	BORDER_MODE_CONTRAST,
	BORDER_MODE_TRIADIC
} BorderMode;

/* Complete configuration structure for the widget toolkit */
typedef struct PixelPrismConfig {
	/* Appearance - Entry (shared config for all entry types) */
	struct EntryBlock {
		char font_family[128];
		int font_size;
		ConfigColor fg;
		ConfigColor bg;
		ConfigColor border;
		ConfigColor valid_border;
		ConfigColor invalid_border;
		ConfigColor focus_border;
		int height; // Auto-calculated, internal use
	} entry_text, entry_int, entry_float, entry_hex;

	/* Appearance - Menu/Menubar */
	struct MenuBlock {
		char font_family[128];
		int font_size;
		ConfigColor fg;
		ConfigColor bg;
		ConfigColor border;
		ConfigColor hover_bg;
		ConfigColor active_bg;
	} menu, menubar;

	/* Appearance - Button (styling only - colors and fonts) */
	struct ButtonBlock {
		char font_family[128];
		int font_size;
		ConfigColor fg;
		ConfigColor bg;
		ConfigColor border;
		ConfigColor hover_border;
		ConfigColor active_border;
	} button;

	/* Appearance - Label (shared styling - colors and fonts only) */
	struct {
		char font_family[128];
		int font_size;
		ConfigColor fg;
		ConfigColor bg;
		ConfigColor border;
	} label;

	/* Label instances - per-instance geometry (5 visual labels) */
	struct {
		int label_hsv_x, label_hsv_y, label_hsv_width, label_hsv_padding, label_hsv_border_width, label_hsv_border_radius, label_hsv_border_enabled;
		int label_hsl_x, label_hsl_y, label_hsl_width, label_hsl_padding, label_hsl_border_width, label_hsl_border_radius, label_hsl_border_enabled;
		int label_rgbf_x, label_rgbf_y, label_rgbf_width, label_rgbf_padding, label_rgbf_border_width, label_rgbf_border_radius, label_rgbf_border_enabled;
		int label_rgbi_x, label_rgbi_y, label_rgbi_width, label_rgbi_padding, label_rgbi_border_width, label_rgbi_border_radius, label_rgbi_border_enabled;
		int label_hex_x, label_hex_y, label_hex_width, label_hex_padding, label_hex_border_width, label_hex_border_radius, label_hex_border_enabled;
	} label_positions;

	/* Entry instances - per-instance geometry (5 visual entries) */
	struct {
		int entry_hsv_x, entry_hsv_y, entry_hsv_width, entry_hsv_padding, entry_hsv_border_width, entry_hsv_border_radius;
		int entry_hsl_x, entry_hsl_y, entry_hsl_width, entry_hsl_padding, entry_hsl_border_width, entry_hsl_border_radius;
		int entry_rgbf_x, entry_rgbf_y, entry_rgbf_width, entry_rgbf_padding, entry_rgbf_border_width, entry_rgbf_border_radius;
		int entry_rgbi_x, entry_rgbi_y, entry_rgbi_width, entry_rgbi_padding, entry_rgbi_border_width, entry_rgbi_border_radius;
		int entry_hex_x, entry_hex_y, entry_hex_width, entry_hex_padding, entry_hex_border_width, entry_hex_border_radius;
	} entry_positions;

	/* Widget geometry - button */
	struct {
		int button_x, button_y;
		int width, height;
		int padding;
		int border_width;
		int hover_border_width;
		int active_border_width;
		int border_radius;
	} button_widget;

	/* Widget geometry - swatch */
	struct {
		int swatch_x, swatch_y;
		int width, height;
		int border_width;
		int border_radius;
	} swatch_widget;

	/* Widget geometry - menubar */
	struct {
		int menubar_x, menubar_y;
		int width;
		int border_width;
		int border_radius;
		int padding;
	} menubar_widget;

	/* Appearance - Tray Menu (styling only - colors and fonts) */
	struct {
		char font_family[64];
		int font_size;
		ConfigColor fg;
		ConfigColor bg;
		ConfigColor hover_bg;
		ConfigColor border;
	} tray_menu;
	
	/* Tray menu widget geometry */
	struct {
		int padding;
		int border_width;
		int border_radius;
	} tray_menu_widget;
	
	/* Appearance - Zoom overlay (colors only) */
	ConfigColor crosshair_color;
	ConfigColor square_color;
	
	/* Zoom overlay widget configuration (visibility/behavior) */
	struct {
		int crosshair_show;
		int square_show;
		int crosshair_show_after_pick;
		int square_show_after_pick;
	} zoom_widget;

	/* Appearance - Main window */
	struct {
		ConfigColor background;
		char font_family[128];
		int font_size;
		ConfigColor text_color;
		ConfigColor link_color;
		int link_underline; /* Underline links in about dialog */
		/* Window dimensions */
		int main_width, main_height;
		int about_width, about_height;
	} main;

	/* Current color state - persists the user's picked/displayed color */
	ConfigColor current_color;

	/* Behavior - Hex formatting */
	int hex_uppercase; /* 0 = lowercase, 1 = uppercase */

	/* Behavior - Swatch */
	BorderMode swatch_border_mode;

	/* Swatch appearance (styling only - colors) */
	struct {
		ConfigColor border;
	} swatch;

	/* Behavior - Cursor */
	int cursor_blink_ms;
	ConfigColor cursor_color;
	int cursor_thickness;

	/* Layout - deprecated, kept for backward compat during transition */
	struct {
		char default_font_family[64];
		int default_font_size;
	} layout;


	/* Behavior - Selection */
	ConfigColor selection_color;
	ConfigColor selection_text_color;
	int undo_depth;

	/* Behavior - Entry max lengths */
	struct {
		int text;
		int integer;
		int floating;
		int hex;
	} max_length;

	/* Behavior - Context menu */
	char menu_items[5][32];
	int menu_item_count;

	/* Paths - External programs */
	char editor_path[256];
	char browser_path[256];

	/* Window Management */
	int remember_position;
	int always_on_top;
	int show_tray_icon;
	int minimize_to_tray;

	/* Clipboard Options */
	int auto_copy;
	char auto_copy_format[8]; /* hex, hsv, hsl, rgb, rgbi */
	int hex_prefix; /* Include # symbol when copying hex */
	int auto_copy_primary; /* Auto-copy selection to PRIMARY */

	/* Change tracking */
	int config_changed; /* 0 = no changes, 1 = unsaved changes */
} PixelPrismConfig;

/* Backward compatibility type aliases */
typedef PixelPrismConfig MiniTheme;
typedef PixelPrismConfig Config; /* Generic config alias for widget independence */
typedef BorderMode SwatchBorderMode;
typedef struct ButtonBlock ButtonBlock;
typedef struct EntryBlock EntryBlock;
typedef struct EntryBlock CssBlock; /* For backward compatibility with css.h */
typedef struct MenuBlock MenuBlock;
/* Note: MenuConfig already exists in menu.h for menu item configuration */

/* Tray menu block type for external use */
typedef struct {
	char font_family[64];
	int font_size;
	ConfigColor fg;
	ConfigColor bg;
	ConfigColor hover_bg;
	ConfigColor border;
	int padding;
	int border_width;
	int border_radius;
} TrayMenuBlock;

/* Enum value aliases */
#define SWATCH_BORDER_COMPLEMENTARY BORDER_MODE_COMPLEMENTARY
#define SWATCH_BORDER_INVERSE BORDER_MODE_INVERSE
#define SWATCH_BORDER_CONTRAST BORDER_MODE_CONTRAST
#define SWATCH_BORDER_TRIADIC BORDER_MODE_TRIADIC

/* ========== INITIALIZATION & LOADING ========== */

/**
 * Initialize configuration with default values
 * @param cfg Pointer to configuration structure
 */
void config_init(PixelPrismConfig *cfg);

/**
 * Load configuration from file
 * @param cfg Pointer to configuration structure
 * @param path Full path to configuration file
 * @return 1 on success, 0 on failure (defaults will be used)
 */
int config_load(PixelPrismConfig *cfg, const char *path);

/**
 * Load configuration from home directory
 * @param cfg Pointer to configuration structure
 * @param home Home directory path
 * @return 1 on success, 0 on failure (defaults will be used)
 */
int config_load_from_home(PixelPrismConfig *cfg, const char *home);

/**
 * Write default configuration to file
 * @param path Full path to configuration file
 * @return 1 on success, 0 on failure
 */
int config_write_defaults(const char *path);
int config_write_defaults_with_values(FILE *f, PixelPrismConfig *cfg);

/* ========== CONFIGURATION MANAGEMENT ========== */

/* Configuration management functions */
void config_init_defaults(PixelPrismConfig *cfg);
int config_load_from_file(PixelPrismConfig *cfg, const char *path);
ConfigColor parse_color(const char *hex_str);

/* ========== CHANGE TRACKING ========== */

/* Change tracking functions */
void config_mark_changed(PixelPrismConfig *cfg);
void config_mark_saved(PixelPrismConfig *cfg);
int config_has_unsaved_changes(PixelPrismConfig *cfg);

/* Border mode utilities */
BorderMode config_get_border_mode(void);
void config_set_border_mode(BorderMode mode);

/* ========== X11 WIDGET UTILITIES ========== */

/**
 * @brief Convert ConfigColor to X11 pixel value
 * @param dpy X11 display connection
 * @param screen Screen number
 * @param color ConfigColor to convert (RGBA float [0.0-1.0])
 * @return Allocated X11 pixel value, or BlackPixel on allocation failure
 *
 * Converts ConfigColor (float RGBA) to X11 pixel by allocating the color
 * in the default colormap. This is a centralized utility used by all widgets
 * for consistent color handling. Components are clamped to [0.0, 1.0] range
 * and converted to 16-bit X11 color components with proper rounding.
 *
 * Thread safety: Not thread-safe (uses XAllocColor)
 * Memory: Returns pixel value, no cleanup needed
 */
unsigned long config_color_to_pixel(Display *dpy, int screen, ConfigColor color);

/**
 * @brief Load Xft font with FontConfig pattern matching
 * @param dpy X11 display connection
 * @param screen Screen number
 * @param family Font family name (e.g., "sans", "monospace", "serif")
 * @param size Font size in pixels
 * @return XftFont pointer, or fallback "sans-14" font if loading fails
 *
 * Uses FontConfig for intelligent font matching with proper DPI awareness,
 * substitutions, and fallback handling. Always returns a valid font - falls
 * back to "sans-14" if the requested font cannot be loaded.
 *
 * The caller is responsible for freeing the font with XftFontClose() when done.
 *
 * Thread safety: Not thread-safe (uses FontConfig and Xft APIs)
 * Memory: Caller must XftFontClose() returned font
 */
XftFont *config_open_font(Display *dpy, int screen, const char *family, int size);

#endif /* CONFIG_H_ */
