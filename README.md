# ctify

`ctify` is a small local music player for Linux written in C. It scans
`~/Music` by default, shows your local tracks in a simple SDL interface, and
uses `mpv` as the playback backend.

Supported audio extensions: `.mp3`, `.flac`, `.ogg`, `.m4a`, `.wav`, `.aac`,
`.opus`, and `.wma`.

## Dependencies

- C compiler
- `make`
- `pkg-config`
- SDL2
- SDL2_ttf
- `mpv`

Examples:

- Arch Linux / Artix Linux: `sudo pacman -S --needed git base-devel pkgconf sdl2 sdl2_ttf mpv`
- Debian / Ubuntu: `sudo apt install git build-essential pkg-config libsdl2-dev libsdl2-ttf-dev mpv`
- Fedora: `sudo dnf install git gcc make pkgconf-pkg-config SDL2-devel SDL2_ttf-devel mpv`
- openSUSE: `sudo zypper install git gcc make pkg-config libSDL2-devel SDL2_ttf-devel mpv`
- Gentoo: `sudo emerge --ask dev-vcs/git sys-devel/gcc sys-devel/make virtual/pkgconfig media-libs/libsdl2 media-libs/sdl2-ttf media-video/mpv`
- Alpine: `sudo apk add git build-base pkgconf sdl2-dev sdl2_ttf-dev mpv`

## Full Install Example

Minimal install:

```sh
git clone https://github.com/Vifuddyxg/ctify
cd ctify
make
sudo make install-desktop
```

Run after installing:

```sh
ctify
ctify /path/to/music
```

Run without installing:

```sh
./ctify
./ctify /path/to/music
```

## Controls

- Click, `Enter`, or `Space`: play selected track
- `Space`: pause/resume when a track is already playing
- Arrow keys: move selection
- Mouse wheel / PageUp / PageDown: scroll
- `/`: search
- `n`: next track
- `b`: previous track
- `+` / `-`: volume up/down
- `r`: rescan library
- `Esc`: close search or quit
