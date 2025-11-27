CC = gcc
PKG_CONFIG_PACKAGES = x11 xrender xft fontconfig freetype2
PKG_CONFIG_CFLAGS = $(shell pkg-config --cflags $(PKG_CONFIG_PACKAGES))
PKG_CONFIG_LIBS = $(shell pkg-config --libs $(PKG_CONFIG_PACKAGES))

CFLAGS = -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -Os -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-stack-protector -fmerge-all-constants -finline-functions-called-once -fomit-frame-pointer -fno-common -std=gnu99 \
	$(PKG_CONFIG_CFLAGS)
LDFLAGS = -Wl,--gc-sections -Wl,--strip-all
LIBS = -lm -lXext -lXpm \
	$(PKG_CONFIG_LIBS)
WARN_CFLAGS =
CFLAGS += $(WARN_CFLAGS)
ASAN_CFLAGS = -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -g -O0 -fsanitize=address -fno-omit-frame-pointer -std=gnu99 $(PKG_CONFIG_CFLAGS)
DEBUG_CFLAGS = -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -g -O0 -std=gnu99 $(PKG_CONFIG_CFLAGS)
DEBUG_TARGET = $(TARGET)_debug
SRC_DIR = src
SRCS = $(SRC_DIR)/button.c $(SRC_DIR)/menu.c $(SRC_DIR)/context.c $(SRC_DIR)/pixelprism.c \
       $(SRC_DIR)/colormath.c $(SRC_DIR)/config.c $(SRC_DIR)/config_registry.c $(SRC_DIR)/entry.c \
       $(SRC_DIR)/swatch.c $(SRC_DIR)/zoom.c $(SRC_DIR)/label.c $(SRC_DIR)/clipboard.c \
       $(SRC_DIR)/tray.c $(SRC_DIR)/icons.c $(SRC_DIR)/dbe.c
OBJS = $(SRCS:.c=.o)
TARGET = pixelprism

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(LIBS) $(LDFLAGS)
	rm -f $(OBJS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(SRC_DIR)/*.o $(TARGET)

# ASAN build for memory debugging
asan:
	@echo "Building with AddressSanitizer..."
	$(MAKE) clean
	$(CC) $(ASAN_CFLAGS) -c button.c -o button.o
	$(CC) $(ASAN_CFLAGS) -c menu.c -o menu.o
	$(CC) $(ASAN_CFLAGS) -c context.c -o context.o
	$(CC) $(ASAN_CFLAGS) -c pixelprism.c -o pixelprism.o
	$(CC) $(ASAN_CFLAGS) -c colormath.c -o colormath.o
	$(CC) $(ASAN_CFLAGS) -c config.c -o config.o
	$(CC) $(ASAN_CFLAGS) -c config_registry.c -o config_registry.o
	$(CC) $(ASAN_CFLAGS) -c entry.c -o entry.o
	$(CC) $(ASAN_CFLAGS) -c swatch.c -o swatch.o
	$(CC) $(ASAN_CFLAGS) -c icons.c -o icons.o
	$(CC) $(ASAN_CFLAGS) -c dbe.c -o dbe.o
	$(CC) $(ASAN_CFLAGS) -c zoom.c -o zoom.o
	$(CC) $(ASAN_CFLAGS) -c label.c -o label.o
	$(CC) $(ASAN_CFLAGS) -c clipboard.c -o clipboard.o
	$(CC) $(ASAN_CFLAGS) -c tray.c -o tray.o
	$(CC) -g -fsanitize=address $(OBJS) -o $(TARGET) $(LIBS)
	@rm -f $(OBJS)
	@echo ""
	@echo "✓ ASAN build complete (with debug symbols)"
	@echo ""
	@echo "Run with leak suppression:"
	@echo "  LSAN_OPTIONS=\"suppressions=\$$(pwd)/asan_suppress.txt\" ./$(TARGET)"
	@echo ""
	@ls -lh $(TARGET)

# Debug build (no sanitizers, full debug info) for Valgrind
.PHONY: debug

debug:
	@echo "Building debug binary (no sanitizers, -g -O0) for Valgrind..."
	$(MAKE) clean
	$(CC) $(DEBUG_CFLAGS) -c button.c -o button.o
	$(CC) $(DEBUG_CFLAGS) -c menu.c -o menu.o
	$(CC) $(DEBUG_CFLAGS) -c context.c -o context.o
	$(CC) $(DEBUG_CFLAGS) -c pixelprism.c -o pixelprism.o
	$(CC) $(DEBUG_CFLAGS) -c colormath.c -o colormath.o
	$(CC) $(DEBUG_CFLAGS) -c config.c -o config.o
	$(CC) $(DEBUG_CFLAGS) -c config_registry.c -o config_registry.o
	$(CC) $(DEBUG_CFLAGS) -c entry.c -o entry.o
	$(CC) $(DEBUG_CFLAGS) -c swatch.c -o swatch.o
	$(CC) $(DEBUG_CFLAGS) -c zoom.c -o zoom.o
	$(CC) $(DEBUG_CFLAGS) -c label.c -o label.o
	$(CC) $(DEBUG_CFLAGS) -c clipboard.c -o clipboard.o
	$(CC) $(DEBUG_CFLAGS) -c tray.c -o tray.o
	$(CC) $(DEBUG_CFLAGS) -c icons.c -o icons.o
	$(CC) $(DEBUG_CFLAGS) -c dbe.c -o dbe.o
	$(CC) $(DEBUG_CFLAGS) $(OBJS) -o $(DEBUG_TARGET) $(LIBS)
	@rm -f $(OBJS)
	@echo ""
	@echo "✓ Debug build complete"
	@echo "  Run under Valgrind, for example:"
	@echo "    valgrind --leak-check=full --show-leak-kinds=definite,indirect --track-origins=yes ./$(DEBUG_TARGET)"
	@echo ""
	@ls -lh $(DEBUG_TARGET)

# Documentation targets
.PHONY: docs docs-clean docs-view

docs:
	@echo "Generating Doxygen documentation with dark theme..."
	doxygen Doxyfile
	@echo "✓ Documentation generated in docs/doxygen/html/"
	@echo "  View with: make docs-view"

docs-clean:
	@echo "Cleaning generated documentation..."
	rm -rf docs/doxygen/
	@echo "✓ Documentation cleaned"

docs-view:
	@echo "Opening documentation in browser..."
	xdg-open docs/doxygen/html/index.html 2>/dev/null || \
	open docs/doxygen/html/index.html 2>/dev/null || \
	echo "Please open docs/doxygen/html/index.html manually"

# Sphinx documentation targets
.PHONY: sphinx sphinx-clean sphinx-view

sphinx:
	@echo "Generating Sphinx documentation..."
	@echo "Step 1: Generating Doxygen XML..."
	doxygen Doxyfile
	@echo "Step 2: Building Sphinx HTML..."
	cd docs/sphinx && sphinx-build -b html . _build/html
	@echo "✓ Sphinx documentation generated in docs/sphinx/_build/html/"
	@echo "  View with: make sphinx-view"

sphinx-clean:
	@echo "Cleaning Sphinx documentation..."
	rm -rf docs/sphinx/_build/ docs/doxygen/xml/
	@echo "✓ Sphinx documentation cleaned"

sphinx-view:
	@echo "Opening Sphinx documentation in browser..."
	xdg-open docs/sphinx/_build/html/index.html 2>/dev/null || \
	open docs/sphinx/_build/html/index.html 2>/dev/null || \
	echo "Please open docs/sphinx/_build/html/index.html manually"
