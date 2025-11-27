# PixelPrism

A lightweight X11 color picker and palette management tool with advanced features.
<img width="590" height="652" alt="screenshot" src="https://github.com/user-attachments/assets/0d405ceb-ebb6-42d6-9284-25b4e60a02de" />

## Features

- **Real-time color picking** with magnified zoom view
- **Multiple color formats**: RGB, HSV, HSL, Hexadecimal
- **Live format conversions** between all color spaces
- **System tray integration** for easy access
- **Clipboard integration** for quick color copying
- **Comprehensive theming system** with hot-reload
- **Configuration file watching** for instant theme updates
- **Color harmony tools** (complementary, triadic, etc.)

## Building

```bash
make
```

## Installation

```bash
make install
```

## Configuration

Configuration file is located at `~/.config/pixelprism/pixelprism.conf`

See the example configuration for all available options.

## Usage

Run `pixelprism` from your application menu or command line.

- **Pick Color Button**: Activates the zoom/magnifier for precise color picking
- **Color Entries**: Display and edit colors in various formats
- **System Tray**: Minimize to tray for easy background operation
- **Menu Bar**: Access application features and information

## Dependencies

- X11 libraries (libX11, libXext, libXpm, libXrender)
- Xft and Fontconfig for text rendering
- Standard C library and math library

## License

Dual-licensed under your choice of:
- MIT License (ALTERNATIVE A)
- Public Domain (ALTERNATIVE B)

See LICENSE file for complete details.

## Author

Created with attention to detail and performance.
