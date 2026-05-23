CC      = gcc
TARGET  = ctify
ALIAS   = citify
SRCDIR  = src
BUILDDIR = build
PREFIX ?= /usr/local

SRCS = $(wildcard $(SRCDIR)/*.c)
OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf sqlite3 2>/dev/null || echo "-I/usr/include/SDL2 -D_REENTRANT")
SDL_LIBS   := $(shell pkg-config --libs sdl2 SDL2_ttf sqlite3 2>/dev/null || echo "-lSDL2 -lSDL2_ttf -lsqlite3")

CFLAGS  = -std=c11 -Wall -Wextra -O2 $(SDL_CFLAGS)
LDFLAGS = $(SDL_LIBS) -lm -lpthread

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
	ln -sf $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(ALIAS)
	install -Dm644 ctify.desktop $(DESTDIR)$(PREFIX)/share/applications/ctify.desktop
	@echo "Installed $(TARGET) to $(DESTDIR)$(PREFIX)/bin/$(TARGET)"
	@echo "Installed $(ALIAS) alias to $(DESTDIR)$(PREFIX)/bin/$(ALIAS)"
	@echo "Installed desktop launcher to $(DESTDIR)$(PREFIX)/share/applications/ctify.desktop"

install-desktop: install
