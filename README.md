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

**Estado:** em desenvolvimento, fase 2b: já é um player mínimo (vídeo
pelo mpv, música pelo motor próprio `kiplay-audio` com gapless e música
de console NSF/SPC/GBS/VGM, playlists em abas, layout configurável). As
legendas do YouTube e o chat ainda não estão ligados ao programa. Veja o
[plano](PLANO.md).

## Uso

```sh
kiplay                      # abre a janela
kiplay video.mkv musica.mp3 # adiciona à playlist da tela e toca o primeiro
kiplay -e *.mp3             # só adiciona (enfileira)
```

Com o Kiplay aberto, `kiplay ARQUIVO` manda o arquivo para a janela que
já existe. Depois do `make install` ele aparece no "Abrir com" dos tipos
de áudio e vídeo.

| Tecla | Faz |
|---|---|
| Espaço, tecla de mídia | tocar / pausar |
| ← / → | volta / avança 5 s (Shift: 1 min) |
| + / − | volume |
| F, F11, duplo clique no vídeo | tela cheia (Esc sai) |
| Enter, duplo clique na lista | toca a faixa |
| Del | remove da playlist |
| Ctrl+T, F2, Ctrl+W | nova playlist, renomear, fechar |
| Alt+1..9 | vai para a playlist |

Configuração em `~/.config/kiplay/kiplay.ini`; por enquanto, o mpv se
ajusta à mão:

```ini
[mpv]
# o programa, ou o caminho completo
program = mpv
# true: lê também o seu ~/.config/mpv/mpv.conf
use_config = false
# opções extras, como no SMPlayer
extra = --hwdec=auto

[playback]
# auto: música no kiplay-audio (leve, gapless); mpv: tudo pelo mpv
engine = auto
# dispositivo ALSA do kiplay-audio
device = default
```

(Comentários só em linha própria: depois do valor, o `#` faria parte
dele.)

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
make run             # roda do build/, com as traduções
sudo make install    # em /usr/local
```

## Licença

GPL-3.0-or-later.
