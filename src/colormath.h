#ifndef COLORMATH_H_
#define COLORMATH_H_

/**
 * @file colormath.h
 *
 * Pure C library for converting between RGB, HSV, and HSL color spaces.
 * All conversions are mathematically accurate and handle edge cases properly.
 *
 * Features:
 * - RGB (0-1 float and 0-255 integer representations)
 * - HSV (Hue-Saturation-Value) color space
 * - HSL (Hue-Saturation-Lightness) color space
 * - Hexadecimal color string parsing and formatting
 * - Fast, branchless algorithms where possible
 *
 * Usage:
 *   RGB to HSV: HSV hsv = rgb_to_hsv((RGBf){r, g, b});
 *   HSV to RGB: RGBf rgb = hsv_to_rgb((HSV){h, s, v});
 *   RGB to HSL: HSL hsl = rgb_to_hsl((RGBf){r, g, b});
 *   HSL to RGB: RGBf rgb = hsl_to_rgb((HSL){h, s, l});
 *   Parse hex: RGB8 color = hex_to_rgb8("#FF5733");
 *   Format hex: hex_from_rgb8((RGB8){255, 87, 51}, buf, sizeof(buf));
 *
 * All RGB values use 0.0-1.0 range (RGBf) or 0-255 (RGB8).
 * Hue is 0-360 degrees, Saturation/Value/Lightness are 0-100 percent.
 *
 * Dependencies:
 * - None (pure standard C with <stdint.h>)
 *
 * This library is completely standalone and can be used independently
 * in any C project requiring color space conversions.
 */

#include <stdint.h>

/* ========== CORE COLOR TYPES ========== */

/* Floating-point RGB color - normalized values [0.0 .. 1.0]
 * Used for mathematical color operations and conversions.
 */
typedef struct {
	double r; /* Red component [0.0 .. 1.0] */
	double g; /* Green component [0.0 .. 1.0] */
	double b; /* Blue component [0.0 .. 1.0] */
} RGBf;

/* 8-bit integer RGB color - values [0 .. 255]
 * Used for display, file I/O, and hex string conversion.
 */
typedef struct {
	uint8_t r; /* Red component [0 .. 255] */
	uint8_t g; /* Green component [0 .. 255] */
	uint8_t b; /* Blue component [0 .. 255] */
} RGB8;

/* HSV (Hue-Saturation-Value) color space
 * Also known as HSB (Hue-Saturation-Brightness).
 * Hue represents color angle, Saturation is color purity, Value is brightness.
 */
typedef struct {
	double H; /* Hue angle [0.0 .. 360.0] degrees */
	double S; /* Saturation [0.0 .. 1.0] */
	double V; /* Value/Brightness [0.0 .. 1.0] */
} HSV;

/* HSL (Hue-Saturation-Lightness) color space
 * Similar to HSV but uses Lightness instead of Value.
 * Lightness represents perceived brightness independent of saturation.
 */
typedef struct {
	double H; /* Hue angle [0.0 .. 360.0] degrees */
	double S; /* Saturation [0.0 .. 1.0] */
	double L; /* Lightness [0.0 .. 1.0] */
} HSL;

/* ========== UTILITY FUNCTIONS ========== */

/* Clamp a value to [0.0 .. 1.0] range
 *
 * @param v Value to clamp
 * @return Clamped value: 0.0 if v < 0.0, 1.0 if v > 1.0, otherwise v
 */
double cm_clamp01(double v);

/* ========== RGB FORMAT CONVERSIONS ========== */

/* Convert 8-bit integer RGB to floating-point RGB
 *
 * Converts [0..255] integer values to [0.0..1.0] float values.
 * Uses precise division by 255.0 for accuracy.
 *
 * @param in 8-bit RGB color
 * @return Normalized floating-point RGB color
 */
RGBf rgb8_to_rgbf(RGB8 in);

/* Convert floating-point RGB to 8-bit integer RGB
 *
 * Converts [0.0..1.0] float values to [0..255] integer values.
 * Clamps out-of-range values and rounds to nearest integer.
 *
 * @param in Normalized floating-point RGB color
 * @return 8-bit RGB color
 */
RGB8 rgbf_to_rgb8(RGBf in);

/* Convert 8-bit RGB to hexadecimal color string
 *
 * Generates uppercase hex string in format "#RRGGBB".
 * Output buffer must be at least 8 bytes (7 chars + null terminator).
 *
 * @param in      8-bit RGB color
 * @param out_hex Output buffer for hex string (min 8 bytes)
 */
void rgb8_to_hex(RGB8 in, char out_hex[8]);

/* Parse hexadecimal color string to 8-bit RGB
 *
 * Accepts formats: "#RRGGBB", "RRGGBB", "#RGB", "RGB"
 * Case-insensitive. Short form (#RGB) expands to #RRGGBB.
 *
 * @param hex Hexadecimal color string to parse
 * @param out Output RGB8 structure (only written if parse succeeds)
 * @return 1 on success, 0 on parse error
 */
int hex_to_rgb8(const char *hex, RGB8 *out);

/* ========== RGB ↔ HSV CONVERSIONS ========== */

/* Convert RGB to HSV color space
 *
 * Converts from RGB (Red-Green-Blue) to HSV (Hue-Saturation-Value).
 * Uses standard conversion algorithm with proper handling of edge cases.
 *
 * @param rgb Normalized RGB color [0.0..1.0]
 * @return HSV color (H: 0-360°, S/V: 0.0-1.0)
 */
HSV rgb_to_hsv(RGBf rgb);

/* Convert HSV to RGB color space
 *
 * Converts from HSV (Hue-Saturation-Value) to RGB (Red-Green-Blue).
 * Handles all hue angles correctly with proper wrapping.
 *
 * @param hsv HSV color (H: 0-360°, S/V: 0.0-1.0)
 * @return Normalized RGB color [0.0..1.0]
 */
RGBf hsv_to_rgb(HSV hsv);

/* ========== RGB ↔ HSL CONVERSIONS ========== */

/* Convert RGB to HSL color space
 *
 * Converts from RGB (Red-Green-Blue) to HSL (Hue-Saturation-Lightness).
 * HSL represents colors in a way more intuitive for lightness adjustments.
 *
 * @param rgb Normalized RGB color [0.0..1.0]
 * @return HSL color (H: 0-360°, S/L: 0.0-1.0)
 */
HSL rgb_to_hsl(RGBf rgb);

/* Convert HSL to RGB color space
 *
 * Converts from HSL (Hue-Saturation-Lightness) to RGB (Red-Green-Blue).
 * Uses standard conversion with proper chroma and hue calculations.
 *
 * @param hsl HSL color (H: 0-360°, S/L: 0.0-1.0)
 * @return Normalized RGB color [0.0..1.0]
 */
RGBf hsl_to_rgb(HSL hsl);

#endif /* COLORMATH_H_ */
