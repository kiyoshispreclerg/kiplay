# Kiplay — notas para o Claude

Player de áudio e vídeo leve para o KiDesktop, em C11 + GTK2: as playlists
em abas do Audacious, a configuração do SMPlayer, vídeo por um backend
externo (mpv, MPlayer) e música num motor próprio (`kiplay-audio`, processo
à parte). O diferencial: legendas do YouTube com estilo (srv3/json3 → ASS)
e chat de live gravado (lateral e danmaku). Leia o `PLANO.md` antes de
começar uma fase (arquitetura, estado, medições) e o `docs/FORMATOS.md`
antes de mexer num leitor.

Fase atual: 0 feita. Próxima: 1 (esqueleto).

O usuário escreve em português (pt-BR) e quer as respostas em português.
Comentários e identificadores no código, em inglês. A interface vai usar
gettext com msgids em inglês (a partir da fase 1).

## Compilar e testar

```sh
make                 # build/libkp.a e os protótipos em build/proto/ (-Werror -Wpedantic)
make test            # testes de unidade (tests/unit/test_*.c)
make asan            # os testes com ASan/UBSan
make lint            # cppcheck + funções proibidas (strcpy, strcat, sprintf, vsprintf, gets)
make BUILD=build/clang CC=clang test   # também tem que passar com clang
build/proto/subconv [-s LxA] [-e fração] [-g contorno] X.srv3 > X.ass
build/proto/embed [--sub X.ass] VIDEO        # mpv embutido no GTK2 (KP_MPV_AO=1: sem som)
build/proto/audio [-d null] [-m] ARQUIVO     # protótipo do kiplay-audio
```

- `testes/` tem os vídeos e legendas reais (fora do git). Não copie nada
  de lá para o repositório: fixtures são sintéticas, no mesmo formato.
- yt-dlp desta máquina: `~/Downloads/yt-dlp_linux-2026.08.19` (não está no
  PATH).
- Janela em teste: num X à parte, `Xvfb :77 -screen 0 1400x900x24 &`,
  `DISPLAY=:77 NO_AT_BRIDGE=1`, captura com
  `python3 -c "from PIL import ImageGrab; ImageGrab.grab(xdisplay=':77').save('x.png')"`.
  No Xvfb o mpv renderiza vídeo por software: memória e CPU do mpv só valem
  medidos no X real.
- Quadros com legenda sem janela: `mpv --no-config --no-audio --start=T
  --frames=1 --vo=image --vo-image-outdir=DIR --sub-file=X.ass VIDEO`.
- Áudio em teste: dispositivo ALSA `null` (o `proto/audio` marca o ritmo
  pelo relógio), `--ao=null` no mpv. Fora disso toca nas caixas do usuário.
- Memória: meça a própria (`Private_Dirty` em `/proc/PID/smaps_rollup`),
  não o RSS.
- `pkill -f <algo>` casa com o próprio shell que roda o comando: use
  `pgrep -x`/`kill PID`.
- Playlists do Audacious do usuário (`~/.config/audacious/playlists/`):
  só leitura, só para os testes de comparação. O Kiplay não as importa.
- Mudar `VERSION` (ou outro `-D` de `config.mk`) não força recompilação:
  `make clean` antes de conferir.

## Estrutura

| Arquivo | Papel |
|---|---|
| `src/ytsub.c` | modelo neutro das legendas do YouTube (pens, posições, estilos, eventos) e os padrões |
| `src/srv3.c` | leitor de srv3 (GMarkup) |
| `src/ytsub_ass.c` | kp_ytsub → ASS: `\an`+`\pos`, cores, bordas; caixa (layer 0) e texto (layer 1) |
| `proto/subconv.c` | srv3 → ASS na linha de comando |
| `proto/embed.c` | mpv `--wid` numa `GtkDrawingArea`, IPC pelo GIO |
| `proto/audio.c` | libavformat/dr_mp3 → ALSA, para medir |
| `docs/FORMATOS.md` | srv3, json3, live_chat.json, audpl como observados |

`src/` vira `build/libkp.a`, sem GTK (GLib sim), que é o que os testes
linkam.

## Convenções

- Prefixo `kp_`/`KP_` em tudo que é público; um módulo por arquivo,
  cabeçalho com comentário explicando o porquê e de quem é a memória.
- Nada bloqueia a interface; quem decodifica roda em outro processo.
- `README.md` (pt-BR, padrão) e `README.en.md` andam juntos: mudou um, mude
  o outro.
- Licença GPL-3.0-or-later; `SPDX-License-Identifier` no topo de cada
  arquivo.
- Commits em português, terminando com a linha Co-Authored-By.
