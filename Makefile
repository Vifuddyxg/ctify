CC      = gcc
TARGET  = ctify
SRCDIR  = src
BUILDDIR = build
PREFIX ?= /usr/local

SRCS = $(wildcard $(SRCDIR)/*.c)
OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf 2>/dev/null || echo "-I/usr/include/SDL2 -D_REENTRANT")
SDL_LIBS   := $(shell pkg-config --libs sdl2 SDL2_ttf 2>/dev/null || echo "-lSDL2 -lSDL2_ttf")

CFLAGS  = -std=c11 -Wall -Wextra -O2 $(SDL_CFLAGS)
LDFLAGS = $(SDL_LIBS) -lm

.PHONY: all clean install install-desktop

all: $(BUILDDIR) $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "Build successful: ./$(TARGET)"

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILDDIR) $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

install-desktop: install
	install -Dm644 ctify.desktop $(DESTDIR)$(PREFIX)/share/applications/ctify.desktop
