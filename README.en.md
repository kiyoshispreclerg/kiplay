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

**Status:** in development, phase 1 (skeleton): the window, the tabbed
playlists and the configurable layout exist, but nothing plays yet. See
the [plan](PLANO.md) (in Portuguese).

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
