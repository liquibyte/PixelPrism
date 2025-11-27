/* colormath.c - Color Space Conversion Implementation
 *
 * Implements mathematically accurate conversions between RGB, HSV, and HSL
 * color spaces using standard algorithms from color science literature.
 *
 * Algorithms based on:
 * - RGB ↔ HSV: Smith's algorithm (1978)
 * - RGB ↔ HSL: CSS3 Color Module specification
 *
 * All functions handle edge cases (achromatic colors, out-of-range values)
 * correctly and use floating-point precision for accuracy.
 */

#include "colormath.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ========== Utility Functions ========== */

/* Clamp a value to [0.0, 1.0] range
 * Uses ternary operator for branchless execution on most compilers.
 */
double cm_clamp01(double v) {
	return (v < 0.0) ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/* ========== RGB Format Conversions ========== */

/* Convert normalized float [0.0, 1.0] to byte [0, 255]
 * Uses lrint() for proper rounding to nearest integer.
 * Handles out-of-range values by clamping to valid byte range.
 */
static inline uint8_t _unit_to_byte(double v) {
	if (v <= 0.0) {
		return 0; // Clamp negative values to 0
	}
	if (v >= 1.0) {
		return 255; // Clamp values >= 1.0 to 255
	}
	return (uint8_t)lrint(v * 255.0); // Round to nearest integer
}

/* Convert 8-bit integer RGB to normalized floating-point RGB
 * Simple division by 255.0 gives exact conversion.
 * No clamping needed as uint8_t is always in valid range.
 */
RGBf rgb8_to_rgbf(RGB8 in) {
	RGBf out = {
		in.r / 255.0, // Red:   [0..255] → [0.0..1.0]
		in.g / 255.0, // Green: [0..255] → [0.0..1.0]
		in.b / 255.0 // Blue:  [0..255] → [0.0..1.0]
	};
	return out;
}

/* Convert normalized floating-point RGB to 8-bit integer RGB
 * Uses _unit_to_byte() which handles clamping and rounding.
 */
RGB8 rgbf_to_rgb8(RGBf in) {
	RGB8 out = {
		_unit_to_byte(in.r), // Convert and clamp red
		_unit_to_byte(in.g), // Convert and clamp green
		_unit_to_byte(in.b) // Convert and clamp blue
	};
	return out;
}

/* Convert RGB to hexadecimal color string
 * Format: "#RRGGBB" with uppercase hex digits.
 * Output buffer must be at least 8 bytes (7 chars + null).
 */
void rgb8_to_hex(RGB8 in, char out_hex[8]) {
	snprintf(out_hex, 8, "#%02X%02X%02X", in.r, in.g, in.b); // Format as #RRGGBB
	out_hex[7] = '\0'; // Ensure null termination (snprintf should do this, but be safe)
}

/* Parse hexadecimal color string to RGB
 * Accepts "#RRGGBB" or "RRGGBB" format (case-insensitive).
 * Returns 1 on success, 0 on parse error or invalid input.
 */
int hex_to_rgb8(const char *hex, RGB8 *out) {
	if (!hex || !out) {
		return 0; // Null pointer check
	}
	if (hex[0] == '#') {
		hex++; // Skip leading '#' if present
	}
	unsigned int r, g, b; // Use unsigned int for sscanf (uint8_t may cause issues)
	if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) != 3) {
		return 0; // Parse failed - invalid format
	}
	// sscanf successful - assign to output structure
	out->r = (uint8_t)r;
	out->g = (uint8_t)g;
	out->b = (uint8_t)b;
	return 1;
}

/* ========== RGB ↔ HSV Conversions ========== */

/* Shared RGB to Hue calculation (used by both HSV and HSL)
 * Computes hue angle, max, min, and delta from RGB components.
 * This is the core algorithm shared between HSV and HSL conversions.
 *
 * @param in Input RGB color (will be clamped to [0,1])
 * @param H  Output: Hue angle in degrees [0, 360)
 * @param M  Output: Max(R, G, B)
 * @param m  Output: Min(R, G, B)
 * @param d  Output: Delta (M - m)
 */
static void _rgb_to_h_common(RGBf in, double *H, double *M, double *m, double *d) {
	double r = cm_clamp01(in.r); // Clamp red to [0,1]
	double g = cm_clamp01(in.g); // Clamp green to [0,1]
	double b = cm_clamp01(in.b); // Clamp blue to [0,1]

	*M = fmax(r, fmax(g, b)); // Find maximum RGB component
	*m = fmin(r, fmin(g, b)); // Find minimum RGB component
	*d = *M - *m; // Delta (chroma)
	if (*d == 0.0) {
		*H = 0.0; // Achromatic (gray) - hue is undefined, set to 0
		return;
	}
	// Calculate hue based on which component is max (standard algorithm)
	if (*M == r) {
		// Red is max: hue in [0°, 60°) or [300°, 360°)
		*H = 60.0 * fmod(((g - b) / *d), 6.0);
	}
	else if (*M == g) {
		// Green is max: hue in [60°, 180°)
		*H = 60.0 * (((b - r) / *d) + 2.0);
	}
	else {
		// Blue is max: hue in [180°, 300°)
		*H = 60.0 * (((r - g) / *d) + 4.0);
	}
	if (*H < 0.0) {
		*H += 360.0; // Normalize negative hue to [0, 360°)
	}
}

/* Convert RGB to HSV color space
 * Uses shared hue calculation, then computes saturation and value.
 * Saturation = chroma / value (0 if value is 0 to avoid division by zero).
 */
HSV rgb_to_hsv(RGBf rgb) {
	double H, M, m, d;
	_rgb_to_h_common(rgb, &H, &M, &m, &d); // Compute hue and chroma
	HSV out = {
		H, // Hue in degrees [0, 360)
		(M > 0.0) ? (d / M) : 0.0, // Saturation (avoid division by zero)
		M // Value is the maximum component
	};
	return out;
}

/* Convert HSV to RGB color space
 * Uses standard chroma-based algorithm with six hue sectors.
 * Each 60° sector has a different RGB component arrangement.
 */
RGBf hsv_to_rgb(HSV hsv) {
	double C = hsv.V * hsv.S; // Chroma (colorfulness)
	double Hs = fmod(fabs(hsv.H), 360.0) / 60.0; // Normalize hue to [0, 6)
	double X = C * (1.0 - fabs(fmod(Hs, 2.0) - 1.0)); // Second largest component
	double r = 0, g = 0, b = 0;
	// Determine RGB based on which 60° hue sector we're in
	if (Hs < 1) { // Red to Yellow (0°-60°)
		r = C;
		g = X;
	}
	else if (Hs < 2) { // Yellow to Green (60°-120°)
		r = X;
		g = C;
	}
	else if (Hs < 3) { // Green to Cyan (120°-180°)
		g = C;
		b = X;
	}
	else if (Hs < 4) { // Cyan to Blue (180°-240°)
		g = X;
		b = C;
	}
	else if (Hs < 5) { // Blue to Magenta (240°-300°)
		r = X;
		b = C;
	}
	else { // Magenta to Red (300°-360°)
		r = C;
		b = X;
	}
	double m = hsv.V - C; // Match value by adding same amount to all components
	RGBf out = {
		cm_clamp01(r + m), // Final red with value adjustment
		cm_clamp01(g + m), // Final green with value adjustment
		cm_clamp01(b + m) // Final blue with value adjustment
	};
	return out;
}

/* ========== RGB ↔ HSL Conversions ========== */

/* Convert RGB to HSL color space
 * Uses shared hue calculation, then computes lightness and saturation.
 * Saturation formula differs from HSV to account for lightness.
 */
HSL rgb_to_hsl(RGBf rgb) {
	double H, M, m, d;
	_rgb_to_h_common(rgb, &H, &M, &m, &d); // Compute hue and chroma
	double L = 0.5 * (M + m); // Lightness is average of max and min
	double S = (d == 0.0) ? 0.0 : (d / (1.0 - fabs(2.0 * L - 1.0))); // Saturation adjusted for lightness
	HSL out = {
		H, // Hue in degrees [0, 360)
		S, // Saturation [0, 1]
		L // Lightness [0, 1]
	};
	return out;
}

/* Helper function for HSL to RGB conversion
 * Converts a hue value (in [0,1] range) to an RGB component.
 * Uses piecewise linear interpolation based on hue position.
 *
 * @param p Lower bound (lightness - chroma/2)
 * @param q Upper bound (lightness + chroma/2)
 * @param t Hue-adjusted parameter in [0,1] (wraps around)
 */
static double _hue2rgb(double p, double q, double t) {
	if (t < 0.0) {
		t += 1.0; // Wrap negative values
	}
	if (t > 1.0) {
		t -= 1.0; // Wrap values > 1
	}
	// Piecewise linear interpolation based on hue sector
	if (t < 1.0 / 6.0) {
		return p + (q - p) * 6.0 * t; // Rising edge
	}
	if (t < 1.0 / 2.0) {
		return q; // Plateau (full saturation)
	}
	if (t < 2.0 / 3.0) {
		return p + (q - p) * (2.0 / 3.0 - t) * 6.0; // Falling edge
	}
	return p; // Base (minimum component)
}

/* Convert HSL to RGB color space
 * Uses helper function _hue2rgb for each RGB component.
 * Special case: achromatic colors (S=0) are simply gray at lightness L.
 */
RGBf hsl_to_rgb(HSL hsl) {
	double H = fmod(fabs(hsl.H), 360.0) / 360.0; // Normalize hue to [0, 1)
	double r, g, b;
	if (hsl.S == 0.0) {
		// Achromatic (gray): all components equal to lightness
		r = g = b = hsl.L;
	}
	else {
		// Chromatic: compute chroma bounds based on lightness
		double q = (hsl.L < 0.5) ?
		           (hsl.L * (1.0 + hsl.S)) : // Dark colors
		           (hsl.L + hsl.S - hsl.L * hsl.S); // Light colors
		double p = 2.0 * hsl.L - q; // Lower bound

		// Compute each RGB component with 120° hue offset
		r = _hue2rgb(p, q, H + 1.0 / 3.0); // Red: +120°
		g = _hue2rgb(p, q, H); // Green: 0° (base)
		b = _hue2rgb(p, q, H - 1.0 / 3.0); // Blue: -120°
	}
	RGBf out = {
		cm_clamp01(r), // Clamp red to [0,1]
		cm_clamp01(g), // Clamp green to [0,1]
		cm_clamp01(b) // Clamp blue to [0,1]
	};
	return out;
}
