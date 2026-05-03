# ctify

`ctify` is a small local music player for Linux written in C. It scans
`~/Music` by default, shows your local tracks in a Soundwave-style SDL
interface, and uses `mpv` as the playback backend.

Supported audio extensions: `.mp3`, `.flac`, `.ogg`, `.m4a`, `.wav`, `.aac`,
`.opus`, and `.wma`.

## Dependencies

- C compiler
- `make`
- `pkg-config`
- SDL2
- SDL2_ttf
- `mpv`
- `ffmpeg` / `ffprobe` for metadata and album covers

Examples:

- Arch Linux / Artix Linux: `sudo pacman -S --needed git base-devel pkgconf sdl2 sdl2_ttf mpv ffmpeg`
- Debian / Ubuntu: `sudo apt install git build-essential pkg-config libsdl2-dev libsdl2-ttf-dev mpv ffmpeg`
- Fedora: `sudo dnf install git gcc make pkgconf-pkg-config SDL2-devel SDL2_ttf-devel mpv ffmpeg`
- openSUSE: `sudo zypper install git gcc make pkg-config libSDL2-devel SDL2_ttf-devel mpv ffmpeg`
- Gentoo: `sudo emerge --ask dev-vcs/git sys-devel/gcc sys-devel/make virtual/pkgconfig media-libs/libsdl2 media-libs/sdl2-ttf media-video/mpv media-video/ffmpeg`
- Alpine: `sudo apk add git build-base pkgconf sdl2-dev sdl2_ttf-dev mpv ffmpeg`

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

- Click a track: select it
- `Space` or `Enter`: play/pause selected track
- `Space`: pause/resume when a track is already playing
- `1` / `2` / `3`: Home, All Songs, Favorites
- Arrow keys: move selection
- Mouse wheel / PageUp / PageDown: scroll
- `/`: search
- `n`: next track
- `b`: previous track
- `f`: favorite/unfavorite selected track
- `a`: add selected track to the `Library` playlist
- `s`: shuffle on/off
- `t`: repeat on/off
- `+` / `-`: volume up/down
- Drag/click the progress bar: seek
- Drag/click the volume bar: set volume
- `r`: rescan library
- `Esc`: close search or quit

## Library State

`ctify` stores lightweight library state in `~/.local/state/ctify`:

- `favorites.txt`: favorite tracks
- `playlists/*.m3u`: playlists shown in the sidebar
- `covers/`: extracted album artwork cache

You can add a playlist manually by creating a `.m3u` file in that folder with
one track path per line.
