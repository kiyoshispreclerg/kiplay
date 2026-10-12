# Kiplay

A lightweight audio and video player for KiDesktop, in C + GTK2. It
combines Audacious's tabbed playlists with SMPlayer's configurability,
plays video through an external program (mpv or MPlayer) and music through
its own small engine.

What sets it apart: showing what yt-dlp downloads alongside a video as
close to the original as possible:

- YouTube captions with their styling, positioning and animation
  (srv3/json3);
- recorded live chat (`live_chat.json`), beside the video or flying across
  it (danmaku);
- ordinary subtitles (srt, ass, vtt...).

**Status:** in development, phase 2: already a minimal player (plays
audio and video through mpv, tabbed playlists, configurable layout).
YouTube captions and chat aren't wired into the program yet. See the
[plan](PLANO.md) (in Portuguese).

## Usage

```sh
kiplay                      # opens the window
kiplay video.mkv song.mp3   # adds to the playlist on screen and plays the first
kiplay -e *.mp3             # only adds (enqueues)
```

With Kiplay open, `kiplay FILE` sends the file to the existing window.
After `make install` it shows up in "Open with" for audio and video types.

| Key | Does |
|---|---|
| Space, media key | play / pause |
| ← / → | back / forward 5 s (Shift: 1 min) |
| + / − | volume |
| F, F11, double click on the video | fullscreen (Esc leaves) |
| Enter, double click on the list | plays the track |
| Del | removes from the playlist |
| Ctrl+T, F2, Ctrl+W | new playlist, rename, close |
| Alt+1..9 | goes to that playlist |

Settings live in `~/.config/kiplay/kiplay.ini`; for now mpv is tuned by
hand:

```ini
[mpv]
# the program, or its full path
program = mpv
# true: also reads your ~/.config/mpv/mpv.conf
use_config = false
# extra options, as in SMPlayer
extra = --hwdec=auto
```

(Comments only on lines of their own: after a value, the `#` would be
part of it.)

To download a video with captions in the right format:

```sh
yt-dlp --write-subs --sub-langs all,live_chat --sub-format srv3/json3/best URL
```

yt-dlp's default format is vtt, which loses positions, colors and sizes.

## Building

Needs GTK 2, GLib, ALSA and the FFmpeg libraries (libavformat, libavcodec,
libswresample), with their `-dev` packages.

```sh
make
make test
make run             # runs from build/, with the translations
sudo make install    # into /usr/local
```

## License

GPL-3.0-or-later.
