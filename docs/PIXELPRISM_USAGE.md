# PixelPrism Usage Guide

## Overview

PixelPrism is a lightweight X11 color picker with advanced features for designers, developers, and digital artists.

## Features

- **Screen Color Picking**: Pixel-perfect color selection with magnified zoom view
- **Multiple Color Formats**: RGB (8-bit/float), HSV, HSL, Hexadecimal
- **Real-time Conversion**: Automatic conversion between all color formats
- **Color Clipboard**: Copy colors to clipboard with a single click
- **System Tray Integration**: Minimize to tray for easy background operation
- **Color History**: Remembers your last picked color across sessions
- **Theming System**: Fully customizable appearance via configuration file
- **Hot-reload Configuration**: Changes to config file are applied instantly

## Quick Start

### Launching

Run from terminal:
```bash
pixelprism
```

Or launch from your application menu.

### Picking a Color

1. Click the **Pick Color** button
2. A magnified zoom view appears - use arrow keys for pixel-perfect positioning
3. Left-click to select the color under the cursor
4. The color appears in all format displays and is automatically saved

### Color Formats

PixelPrism displays your selected color in multiple formats (top to bottom in the UI):

- **HSV** (Hue, Saturation, Value): `24.0° 75.0% 100.0%`
- **HSL** (Hue, Saturation, Lightness): `24.0° 100.0% 62.7%`
- **RGB Float** (0.0-1.0): `1.000, 0.502, 0.251`
- **RGB Integer** (0-255): `255, 128, 64`
- **Hexadecimal**: `#FF8040`

### Copying Colors

Each format field supports:
- **Auto-copy**: Enable in config to automatically copy when you pick
- **Manual copy**: Click a format entry and use Ctrl+C
- **Paste**: Paste hex colors into the hex field with Ctrl+V

## Configuration

Configuration file: `~/.config/pixelprism/pixelprism.conf`

The file is created with defaults on first run. Edit it to customize:

- Window size and position
- Colors and fonts for all UI elements
- Border widths and radii
- Zoom crosshair and square visibility
- Auto-copy behavior
- System tray behavior
- Color harmony mode (for the swatch border)

Changes are applied automatically when you save the file.

## Keyboard Shortcuts

### While Picking Colors

- **Arrow Keys**: Move cursor pixel-by-pixel
- **Left Click**: Pick color at cursor
- **Right Click**: Cancel picking
- **Escape**: Cancel picking

### Main Window

- **Tab**: Cycle through color format fields
- **Ctrl+C**: Copy selected field
- **Ctrl+V**: Paste hex color
- **Q**: Quit application

## Menu Bar

### File Menu

- **Exit**: Close the application

### Edit Menu

- **Configuration**: Open config file in your default text editor
- **Reset**: Reset all color displays to black (#000000)

### About Menu

- **PixelPrism**: Display version and author information

## System Tray

Click the system tray icon to:
- **Show/Hide**: Toggle main window visibility
- **Pick Color**: Activate color picker
- **About**: Show application info
- **Quit**: Exit the application

## Tips

1. **Precise Selection**: Use arrow keys while zooming for pixel-perfect color picking
2. **Quick Access**: Keep PixelPrism in the system tray for instant access
3. **Format Conversion**: Use Ctrl+C to copy any format field
4. **Persistent Colors**: Your last picked color is saved and restored on next launch
5. **Custom Themes**: Create your own theme by editing the config file

## Troubleshooting

**Zoom doesn't appear**:
- Ensure you have X11 permissions
- Check that no other application is grabbing the display

**System tray icon missing**:
- Ensure your desktop environment has a system tray
- Some minimal window managers may not support system tray icons

**Config changes not applying**:
- Check file syntax - invalid values are ignored
- Look for errors in terminal output

## Advanced Usage

### Color Harmony

The color swatch border uses harmonious colors based on the current selection. Configure the harmony mode in the config file ([swatch-widget] section, border-mode key):
- `complementary`: Opposite color on the color wheel
- `triadic`: Three evenly-spaced colors
- `inverse`: Luminance inversion
- `contrast`: High contrast black/white

### Custom Themes

Edit `pixelprism.conf` to create custom themes. All colors, fonts, and dimensions are configurable. See the config file comments for details.
