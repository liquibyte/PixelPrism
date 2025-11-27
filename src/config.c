/* config.c - Configuration Utilities (Widget-Independent)
 *
 * Provides pure utility functions for color parsing, font loading, and X11
 * color conversion. These are widget-independent helpers used across the toolkit.
 *
 * Application-specific configuration logic (loading/saving/parsing PixelPrism
 * config files) has been moved to pixelprism.c to eliminate circular dependencies.
 */

#include "config.h"
#include <string.h>
#include <stdio.h>
#include <fontconfig/fontconfig.h>

/* ========== BORDER MODE STATE ========== */

static BorderMode current_border_mode = BORDER_MODE_COMPLEMENTARY;

BorderMode config_get_border_mode(void) {
	return current_border_mode;
}

void config_set_border_mode(BorderMode mode) {
	current_border_mode = mode;
}

/* ========== COLOR UTILITIES ========== */

/**
 * @brief Parse hex color string #RRGGBB or #RRGGBBAA
 *
 * See config.h for full documentation.
 * Shared utility function for parsing color strings across all widgets.
 */
ConfigColor parse_color(const char *hex_str) {
	ConfigColor color = {0.0, 0.0, 0.0, 1.0};
	
	if (!hex_str || strlen(hex_str) < 7) {
		return color;
	}
	
	// Skip # if present
	const char *start = hex_str;
	if (hex_str[0] == '#') {
		start = hex_str + 1;
	}
	
	// Parse hex values
	unsigned int r, g, b;
	if (sscanf(start, "%02x%02x%02x", &r, &g, &b) == 3) {
		color.r = (double)r / 255.0;
		color.g = (double)g / 255.0;
		color.b = (double)b / 255.0;
	}
	
	return color;
}

/* ========== X11 WIDGET UTILITIES ========== */

/**
 * @brief Convert float [0.0-1.0] to X11 color component [0-65535]
 * Internal helper for config_color_to_pixel()
 */
static unsigned short color_component_to_x11(double c) {
	// Clamp to valid range [0.0, 1.0]
	if (c < 0.0) c = 0.0;
	if (c > 1.0) c = 1.0;
	
	// Convert to X11 16-bit color component with rounding
	return (unsigned short)(c * 65535.0 + 0.5);
}

/**
 * @brief Convert ConfigColor to X11 pixel value
 *
 * See config.h for full documentation.
 * Shared utility function used by all widgets for color conversion.
 */
unsigned long config_color_to_pixel(Display *dpy, int screen, ConfigColor color) {
	XColor xc = {0};
	Colormap cmap = DefaultColormap(dpy, screen);
	
	// Convert ConfigColor float components [0.0-1.0] to X11 components [0-65535]
	xc.red   = color_component_to_x11(color.r);
	xc.green = color_component_to_x11(color.g);
	xc.blue  = color_component_to_x11(color.b);
	xc.flags = DoRed | DoGreen | DoBlue;
	
	// Allocate color in colormap (handles visual/depth conversion)
	if (!XAllocColor(dpy, cmap, &xc)) {
		// Allocation failed (colormap full?), return black as safe fallback
		return BlackPixel(dpy, screen);
	}
	
	return xc.pixel;
}

/**
 * @brief Load Xft font with FontConfig pattern matching
 *
 * See config.h for full documentation.
 * Shared utility function used by all text-rendering widgets.
 */
XftFont *config_open_font(Display *dpy, int screen, const char *family, int size) {
	// Parse font family name, use "sans" if NULL or empty
	FcPattern *pat = FcNameParse((const FcChar8 *)(family && family[0] ? family : "sans"));
	if (!pat) {
		return XftFontOpenName(dpy, screen, "sans-14");
	}
	
	// Add pixel size to pattern
	FcPatternAddInteger(pat, FC_PIXEL_SIZE, size);
	
	// Apply FontConfig substitutions (handles DPI, family aliases, etc.)
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	
	// Match best available font from pattern
	FcResult result;
	FcPattern *match = FcFontMatch(NULL, pat, &result);
	FcPatternDestroy(pat);
	
	if (!match) {
		return XftFontOpenName(dpy, screen, "sans-14");
	}
	
	// Open the matched font
	XftFont *font = XftFontOpenPattern(dpy, match);
	if (!font) {
		// Font opening failed, clean up match and use fallback
		// Note: XftFontOpenPattern takes ownership of match only on success
		FcPatternDestroy(match);
		return XftFontOpenName(dpy, screen, "sans-14");
	}
	
	// Success! XftFontOpenPattern has taken ownership of match pattern
	return font;
}
