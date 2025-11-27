# PixelPrism Configuration Guide

## Configuration File Location

`~/.config/pixelprism/pixelprism.conf`

The file is automatically created with defaults on first run.

## File Format

The configuration file uses a simple INI-style format:

```ini
[section]
key = value
```

Comments start with `#` and blank lines are ignored. Configuration is divided into two main groups:

1. **VISUAL STYLING** - Colors, fonts, and appearance
2. **CONFIGURATION & BEHAVIOR** - Widget geometry, positioning, and behavior

## Visual Styling Sections

### [button]

Pick Color button styling.

```ini
[button]
active-border = #1C70D7
background = #FFFFFF
border = #CDC6C2
color = #2D3436
font-family = DejaVu Sans
font-size = 14
hover-border = #619FEA
```

- **color**: Button text color
- **background**: Button background
- **border**: Normal border color
- **hover-border**: Border when mouse hovers
- **active-border**: Border when pressed
- **font-family**: Font name
- **font-size**: Font size in points

### [context-menu] / [menubar]

Context menu and menubar styling.

```ini
[menubar]
active-background = #000000
background = #FFFFFF
border = #CDC6C2
color = #2D3436
font-family = DejaVu Sans
font-size = 14
hover-background = #E0DEDB
```

- **active-background**: Background when menu item is selected
- **hover-background**: Background when mouse hovers over item

### [entry-text], [entry-int], [entry-float], [entry-hex]

Text entry fields for different color value formats. HSV and HSL fields use [entry-text], RGB Integer uses [entry-int], RGB Float uses [entry-float], and Hexadecimal uses [entry-hex]. All entry sections have the same structure:

```ini
[entry-hex]
background = #FFFFFF
border = #CDC6C2
color = #2D3436
focus-border = #CDC6C2
font = DejaVu Sans
font-size = 16
invalid-border = #DF1B23
valid-border = #25A169
```

- **focus-border**: Border when field is focused
- **valid-border**: Border color for valid input (green)
- **invalid-border**: Border color for invalid input (red)

### [label]

Static text labels above entry fields.

```ini
[label]
background = #F6F5F4
border = #CDC6C2
color = #2D3436
font = DejaVu Sans
font-size = 16
```

Note: Individual label widgets have their own geometry sections ([label-widget-*])

### [swatch]

Color display swatch border styling.

```ini
[swatch]
border = #CDC7C2
```

- **border**: Swatch border color (can use color harmony modes - see [swatch-widget])

### [tray-menu]

System tray icon menu styling.

```ini
[tray-menu]
background = #F6F5F4
border = #CDC6C2
color = #2D3436
font-family = DejaVu Sans
font-size = 14
hover-background = #E0DEDB
```

### [zoom]

Magnifier/zoom view colors.

```ini
[zoom]
crosshair-color = #00FF00
square-color = #FF0000
```

- **crosshair-color**: Color of crosshair overlay
- **square-color**: Color of center square indicator

## Configuration & Behavior Sections

### [behavior]

Application behavior settings.

```ini
[behavior]
always-on-top = true
cursor-blink-ms = 700
cursor-color = #3584E3
cursor-width = 1
hex-case = upper
minimize-to-tray = true
remember-position = true
show-tray-icon = true
undo-depth = 64
```

- **always-on-top**: Keep window above other windows (true/false)
- **cursor-blink-ms**: Text cursor blink rate in milliseconds (0 = no blink)
- **cursor-color**: Text cursor color
- **cursor-width**: Cursor width in pixels
- **hex-case**: Hex color output format (upper/lower)
- **minimize-to-tray**: Minimize to system tray instead of taskbar
- **remember-position**: Remember window position across sessions
- **show-tray-icon**: Show system tray icon
- **undo-depth**: Number of undo levels (default 64)

### [clipboard]

Clipboard automation settings.

```ini
[clipboard]
auto-copy = false
auto-copy-format = hex
auto-copy-primary = true
hex-prefix = true
```

- **auto-copy**: Automatically copy picked colors to clipboard
- **auto-copy-format**: Format to copy (hex, hsv, hsl, rgb, rgbi)
- **auto-copy-primary**: Copy to X11 PRIMARY selection (middle-click paste)
- **hex-prefix**: Include '#' prefix when copying hex values

### [main]

Main window dimensions and styling.

```ini
[main]
about-height = 300
about-width = 590
background = #F6F5F4
color = #2D3436
font = DejaVu Sans
font-size = 14
link-color = #1C70D7
link-underline = true
main-height = 300
main-width = 590
```

- **main-width**, **main-height**: Main window dimensions in pixels
- **about-width**, **about-height**: About dialog dimensions
- **background**: Main window background color
- **color**: Main text color
- **link-color**: Color for clickable links in About dialog
- **link-underline**: Whether to underline links (true/false)

### [paths]

External application paths.

```ini
[paths]
browser = /usr/bin/xdg-open
editor = /usr/bin/geany
```

- **browser**: Web browser command for opening URLs
- **editor**: Text editor command for editing config files

### Widget Geometry Sections

Individual sections control widget placement and geometry:

#### [button-widget]

```ini
[button-widget]
active-border-width = 1
border-radius = 4
border-width = 1
button-x = 492
button-y = 255
height = 32
hover-border-width = 1
padding = 8
width = 88
```

#### [entry-widget-hex], [entry-widget-hsl], [entry-widget-hsv], [entry-widget-rgbf], [entry-widget-rgbi]

```ini
[entry-widget-hex]
border-radius = 4
border-width = 1
entry-hex-x = 383
entry-hex-y = 180
padding = 4
width = 197
```

#### [label-widget-hex], [label-widget-hsl], [label-widget-hsv], [label-widget-rgbf], [label-widget-rgbi]

```ini
[label-widget-hex]
border-enabled = false
border-radius = 0
border-width = 1
label-hex-x = 310
label-hex-y = 180
padding = 4
width = 60
```

#### [menubar-widget]

```ini
[menubar-widget]
border-radius = 4
border-width = 1
menubar-x = 306
menubar-y = 0
padding = 4
width = 278
```

#### [swatch-widget]

```ini
[swatch-widget]
border-mode = complementary
border-radius = 4
border-width = 1
height = 74
swatch-x = 310
swatch-y = 215
width = 74
```

- **border-mode**: Color harmony calculation for swatch border
  - `complementary`: Opposite color on the color wheel
  - `triadic`: Three evenly-spaced colors
  - `inverse`: Luminance inversion
  - `contrast`: High contrast black/white

#### [tray-menu-widget]

```ini
[tray-menu-widget]
border-radius = 4
border-width = 1
padding = 2
```

#### [zoom-widget]

```ini
[zoom-widget]
crosshair-show = true
crosshair-show-after-pick = false
square-show = true
square-show-after-pick = true
```

- **crosshair-show**: Show crosshair in zoom view
- **crosshair-show-after-pick**: Keep crosshair visible after picking
- **square-show**: Show center square indicator
- **square-show-after-pick**: Keep square visible after picking

## Color Format

All colors use CSS-style hex format:

- **#RRGGBB**: RGB (e.g., `#FF8040`)
- **#RRGGBBAA**: RGBA with alpha (e.g., `#FF804080` = 50% transparent)

Values are hexadecimal (00-FF for each component).

## Tips

1. **Backup**: Copy your config before major changes
2. **Live Reload**: Changes apply instantly when you save
3. **Validation**: Invalid values are ignored and defaults are used
4. **Comments**: Use `#` to comment out lines for experimentation
5. **Colors**: Use PixelPrism itself to pick colors\!

## Example: Creating a Dark Theme

```ini
# Dark theme colors
[main]
color = #E0E0E0
background = #1E1E1E

[button]
color = #FFFFFF
background = #2D2D30
border = #555555
hover-border = #777777

[entry-hex]
color = #E0E0E0
background = #252526
border = #3E3E42
valid-border = #4EC9B0
invalid-border = #F48771

[label]
color = #CCCCCC
background = #1E1E1E

[menubar]
color = #E0E0E0
background = #2D2D30
hover-background = #3E3E42
```

## Resetting Configuration

Delete `~/.config/pixelprism/pixelprism.conf` and restart PixelPrism to regenerate defaults.
