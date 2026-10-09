# Kiplay

Player de áudio e vídeo leve para o KiDesktop, em C + GTK2. Junta as
playlists em abas do Audacious com a configuração do SMPlayer, toca vídeo
por um programa externo (mpv ou MPlayer) e música num motor próprio e
pequeno.

O diferencial: mostrar o que o yt-dlp baixa junto do vídeo o mais perto
possível do original:

- legendas do YouTube com estilo, posição e animação (srv3/json3);
- chat de live gravado (`live_chat.json`), ao lado do vídeo ou atravessando
  a tela (danmaku);
- as legendas comuns (srt, ass, vtt...).

**Estado:** em desenvolvimento, fase 0 (investigação). Ainda não há
programa para usar; veja o [plano](PLANO.md).

Para baixar um vídeo com as legendas no formato certo:

```sh
yt-dlp --write-subs --sub-langs all,live_chat --sub-format srv3/json3/best URL
```

O formato padrão do yt-dlp é vtt, que perde posição, cores e tamanhos.

## Compilar

Precisa de GTK 2, GLib, ALSA e das bibliotecas do FFmpeg (libavformat,
libavcodec, libswresample), com os pacotes `-dev`.

```sh
make
make test
```

## Licença

GPL-3.0-or-later.
