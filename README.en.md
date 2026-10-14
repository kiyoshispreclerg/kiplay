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

**Status:** in development, phase 5: already an everyday player (video
through mpv, music through its own `kiplay-audio` engine with gapless and
NSF/SPC/GBS/VGM game music; tabbed playlists with tags, a play queue,
search, drag to reorder, m3u/pls import and export; configurable layout;
tabbed preferences, editable shortcuts, resume where it stopped, audio
and subtitle tracks, MPRIS, tray icon, compact mode; YouTube captions with
their style and position, as on the site). The saved chat isn't wired into
the program yet. See
the [plan](PLANO.md) and the [comparison with Audacious](docs/BENCH.md)
(both in Portuguese).

## Usage

```sh
kiplay                      # opens the window
kiplay video.mkv song.mp3   # adds to the playlist on screen and plays the first
kiplay ~/Music/Album        # folders go in whole, by name
kiplay -e *.mp3             # only adds (enqueues)
```

With Kiplay open, `kiplay FILE` sends the file to the existing window.
After `make install` it shows up in "Open with" for audio and video types.

| Key | Does |
|---|---|
| Space, media key | play / pause |
| ← / → | back / forward 5 s (Shift: 1 min) |
| Ctrl+← / Ctrl+→, < / > | previous / next track |
| + / −, M | volume, mute |
| F, F11, double click on the video | fullscreen (Esc leaves) |
| Ctrl+C | compact mode: only the video and the controls |
| # , J, V | next audio track, next subtitle, show/hide subtitles |
| Ctrl++ / Ctrl+−, X / Z | audio delay, subtitle delay (100 ms) |
| A | next aspect ratio (4:3, 16:9...) |
| Enter, double click on the list | plays the track |
| Del | removes from the playlist |
| Q | queues (or unqueues); the queue plays before the rest |
| Ctrl+J | jump to file: search the playlist on screen |
| Ctrl+M, Ctrl+R, Ctrl+S | stop after this track, repeat, shuffle |
| Alt+↑ / Alt+↓, drag | moves the selected tracks |
| Ctrl+O, Ctrl+Shift+O | add files, add folder |
| Ctrl+T, F2, Ctrl+W | new playlist, rename, close |
| Ctrl+P | preferences |
| Alt+1..9 | goes to that playlist |

Shortcuts are changed in Options → Preferences → Shortcuts (up to two
keys per command). A right click on the video or the control bar opens a
menu with the essentials (handy in compact mode and fullscreen).

Kiplay remembers, for each video, where it stopped, the audio and
subtitle tracks chosen, the delays and the aspect ratio (in
`~/.local/share/kiplay/files/`), and goes on from there next time.
Preferences → General extends this to music or turns it off.

The columns (number, artist, album, length, path) are under View →
Playlist columns. Tags are read in the background, in a separate process,
and kept in the playlist.

Kiplay shows up to the desktop through MPRIS (media keys, applets,
kikarinhas) and can have a tray icon.

Settings live in `~/.config/kiplay/kiplay.ini`, which the preferences
dialog writes and which can also be edited by hand (comments are kept;
only what differs from the default is written):

```ini
[mpv]
# the program, or its full path
program = mpv
# true: also reads your ~/.config/mpv/mpv.conf
use_config = false
# extra options, as in SMPlayer
extra = --deband

[video]
hwdec = auto-safe

[subtitles]
languages = pt-BR,pt,en
font_size = 48

[playback]
# auto: music on kiplay-audio (light, gapless); mpv: everything on mpv
engine = auto
# ALSA device for kiplay-audio
device = default

[files]
# off, video or all: resume where it stopped
resume = video

[shortcuts]
# keys in GTK's format; empty: none
next = <Control>Right,greater
mute =
```

(Comments only on lines of their own: after a value, the `#` would be
part of it.)

To download a video with captions in the right format:

```sh
yt-dlp --write-subs --sub-langs all,live_chat --sub-format srv3/json3/best URL
```

yt-dlp's default format is vtt, which loses positions, colors and sizes.

Kiplay finds the files next to the video by name (`Title [ID].mp4` →
`Title [ID].ko.srv3`, `.json3`, `.vtt`, `.srt`, `.ass`...). For each
language the srv3 (or json3) wins over the plain ones: it is converted to
ASS (in `~/.cache/kiplay/subs`) and shows in the Subtitles menu as "YouTube
style". The language is picked from the `[subtitles] languages` list. To
convert by hand, for another player:

```sh
kiplay --convert "Title [ID].ko.srv3" > ko.ass
```

In `[subtitles]`, `youtube_edge = strong` swaps YouTube's thin outline for a
thick, blurred one that is easier to read; `auto = no` turns the search off.

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
