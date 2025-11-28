CC = gcc
PKG_CONFIG_PACKAGES = x11 xrender xft fontconfig freetype2
PKG_CONFIG_CFLAGS = $(shell pkg-config --cflags $(PKG_CONFIG_PACKAGES))
PKG_CONFIG_LIBS = $(shell pkg-config --libs $(PKG_CONFIG_PACKAGES))

CFLAGS = -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-stack-protector -fmerge-all-constants -finline-functions-called-once -fomit-frame-pointer -fno-common -std=gnu99 \
	$(PKG_CONFIG_CFLAGS)
LDFLAGS = -Wl,--gc-sections -Wl,--strip-all
LIBS = -lm -lXext -lXpm \
	$(PKG_CONFIG_LIBS)

SRC_DIR = src
SRCS = $(SRC_DIR)/button.c $(SRC_DIR)/menu.c $(SRC_DIR)/context.c $(SRC_DIR)/pixelprism.c \
       $(SRC_DIR)/colormath.c $(SRC_DIR)/config.c $(SRC_DIR)/config_registry.c $(SRC_DIR)/entry.c \
       $(SRC_DIR)/swatch.c $(SRC_DIR)/zoom.c $(SRC_DIR)/label.c $(SRC_DIR)/clipboard.c \
       $(SRC_DIR)/tray.c $(SRC_DIR)/icons.c $(SRC_DIR)/dbe.c
OBJS = $(SRCS:.c=.o)
TARGET = pixelprism
PREFIX = /usr
BINDIR = $(PREFIX)/bin
DESKTOPDIR = $(PREFIX)/share/applications
PIXMAPDIR = $(PREFIX)/share/pixmaps
DESKTOPFILE = PixelPrism.desktop

all: $(TARGET) pixelprism.xpm

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(LIBS) $(LDFLAGS)
	rm -f $(OBJS)

# Ensure icon is built before installation
install: icon

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(SRC_DIR)/*.o $(TARGET) pixelprism.xpm

# Extract icon from src/icons.c
icon: pixelprism.xpm

pixelprism.xpm: $(SRC_DIR)/icons.c
	@echo "Extracting icon from $<..."
	@sed -n '/^\/\* XPM \*\//,/^};/p' $< > $@
	@if [ ! -s $@ ]; then \
		echo "❌ Failed to extract XPM data"; \
		rm -f $@; \
		exit 1; \
	else \
		echo "✓ Icon extracted as $@"; \
	fi

install: $(TARGET) $(DESKTOPFILE)
	install -Dm755 $(TARGET) $(BINDIR)/$(TARGET)
	install -Dm644 $(DESKTOPFILE) $(DESKTOPDIR)/$(DESKTOPFILE)
	install -Dm644 pixelprism.xpm $(PIXMAPDIR)/pixelprism.xpm
	@echo "✓ Installed PixelPrism and desktop entry"

uninstall:
	rm -f $(BINDIR)/$(TARGET)
	rm -f $(DESKTOPDIR)/$(DESKTOPFILE)
	rm -f $(PIXMAPDIR)/pixelprism.xpm
	@echo "✓ Uninstalled PixelPrism"
