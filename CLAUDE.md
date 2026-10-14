# Kiplay — notas para o Claude

Player de áudio e vídeo leve para o KiDesktop, em C11 + GTK2: as playlists
em abas do Audacious, a configuração do SMPlayer, vídeo por um backend
externo (mpv, MPlayer) e música num motor próprio (`kiplay-audio`, processo
à parte). O diferencial: legendas do YouTube com estilo (srv3/json3 → ASS)
e chat de live gravado (lateral e danmaku). Leia o `PLANO.md` antes de
começar uma fase (arquitetura, estado, medições) e o `docs/FORMATOS.md`
antes de mexer num leitor.

Fase atual: 5 feita (sidecars e legendas do YouTube: srv3/json3 → ASS,
menu Legendas, `--convert`). Próxima: 6 (chat lateral).

O usuário escreve em português (pt-BR) e quer as respostas em português.
Comentários e identificadores no código, em inglês. A interface é
multi-idiomas com gettext: todo texto que uma pessoa lê vai em `_("...")`
com msgid em **inglês** (`N_()` para tabelas, `ngettext()` para plurais),
e as traduções ficam em `po/` (por enquanto só `pt_BR`). Mexeu em texto?
`make pot` e traduza o que aparecer vazio no `po/pt_BR.po`
(`tools/fill_po.py po/pt_BR.po X.json` ajuda em lote). Arquivo novo com
textos vai no `po/POTFILES.in`. Só o `kp_log_debug` (do `-v`) fica sem
tradução.

## Compilar e testar

```sh
make                 # build/kiplay, build/locale e os protótipos (-Werror -Wpedantic)
make test            # testes de unidade (tests/unit/test_*.c)
make asan            # os testes com ASan/UBSan
make asan-run ARGS=--exit-after-map   # o programa com ASan (supressões em tools/lsan.supp)
make pot             # recria po/kiplay.pot e mescla nos .po
build/kiplay -v [-c X.ini] [ARQ...]   # -v: tempos, linhas do mpv (mpv<), eventos, XID do vídeo
make lint            # cppcheck + funções proibidas (strcpy, strcat, sprintf, vsprintf, gets)
make BUILD=build/clang CC=clang test   # também tem que passar com clang
build/proto/subconv [-s LxA] [-e fração] [-g contorno] X.srv3 > X.ass
build/proto/embed [--sub X.ass] VIDEO        # mpv embutido no GTK2 (KP_MPV_AO=1: sem som)
build/proto/audio [-d null] [-m] ARQUIVO     # protótipo da fase 0 (o motor de verdade é o build/kiplay-audio)
printf 'tick 1\nload 0 ARQ\n' | build/kiplay-audio -d null   # o motor à mão (protocolo em audio/cmd.h)
tools/bench-audio.sh [-d null|default] [-s S] ARQ...       # motor × mpv: memória e CPU
printf 'ARQ\n' | build/kiplay-audio --tags                   # o leitor de tags à mão
tools/audpl2m3u8.py [--no-tags] DIR    # playlists do Audacious → DIR/kiplay (XDG_CONFIG_HOME de teste)
PULSE_SINK=kpbench tools/bench-compare.sh -k DIR -a CÓPIA[:nome] CENÁRIOS.tsv   # Kiplay × Audacious
```

- `testes/` tem os vídeos e legendas reais (fora do git). Não copie nada
  de lá para o repositório: fixtures são sintéticas, no mesmo formato.
- yt-dlp desta máquina: `~/Downloads/yt-dlp_linux-2026.08.19` (não está no
  PATH).
- Janela em teste: num X à parte, `Xvfb :77 -screen 0 1400x900x24 &`,
  `DISPLAY=:77 XDG_CONFIG_HOME=<scratchpad>/cfg NO_AT_BRIDGE=1
  GTK2_RC_FILES=` (o `~/.gtkrc-2.0` do usuário carrega o
  `appmenu-gtk-module`, que tira a barra de menus da janela), captura com
  `python3 -c "from PIL import ImageGrab; ImageGrab.grab(xdisplay=':77').save('x.png')"`.
  No Xvfb o mpv renderiza vídeo por software: memória e CPU do mpv só valem
  medidos no X real. Teclado no Xvfb (sem gerenciador de janelas): clique
  na janela com `xdotool mousemove --window W X Y click 1` e mande as teclas
  com `xdotool key ...` sem `--window` (o `--window` usa XSendEvent, que o
  GTK ignora).
- Quadros com legenda sem janela: `mpv --no-config --no-audio --start=T
  --frames=1 --vo=image --vo-image-outdir=DIR --sub-file=X.ass VIDEO`.
- O Kiplay de teste entra no barramento de sessão (MPRIS) e grava opções
  por arquivo em `~/.local/share/kiplay/files/`: rode com `dbus-launch`
  próprio (`eval $(dbus-launch --sh-syntax)`; senão ele aparece para as
  teclas de mídia e applets do usuário) e `XDG_DATA_HOME=<scratchpad>/data`.
  `test_mpris` usa um `dbus-daemon` próprio (`GTestDBus`).
- Kiplay tocando em teste: `XDG_CONFIG_HOME=<scratchpad>/cfg` com
  `[mpv] extra = --ao=null` no `kiplay.ini` (sem som). A instância única
  usa um socket por pasta de configuração, então o Kiplay de teste nunca
  manda arquivos para o do usuário. `test_mpv` roda um mpv de verdade
  (`--ao=null --vo=null`); mate só o PID dele (`kp_backend_pid`), nunca
  `pkill mpv` (pode haver um do usuário).
- Gerenciador de janelas no Xvfb (para tela cheia, foco): `openbox
  --sm-disable &`; a janela fica centralizada e decorada, use
  `xdotool search --onlyvisible --name '^Kiplay$'` e `windowactivate`.
- Títulos em japonês na playlist custam ~5–8 MB (fontes CJK no Pango):
  para medir o Kiplay em si, use arquivos de nome ASCII.
- Áudio em teste: dispositivo ALSA `null` (o `kiplay-audio` e o
  `proto/audio` marcam o ritmo pelo relógio), `--ao=null` no mpv; no
  Kiplay, `[playback] device = null` e `[mpv] extra = --ao=null`. Fora
  disso toca nas caixas do usuário (o `bench-audio.sh -d default` toca em
  volume 0). `test_audio` roda o motor de verdade no `null`.
- Memória: meça a própria (`Private_Dirty` em `/proc/PID/smaps_rollup`),
  não o RSS.
- `pkill -f <algo>` casa com o próprio shell que roda o comando: use
  `pgrep -x`/`kill PID`.
- Playlists do Audacious do usuário (`~/.config/audacious/playlists/`):
  só leitura, só para os testes de comparação. O Kiplay não as importa.
  O Audacious de teste roda numa **cópia** de `~/.config/audacious`
  (ele grava a configuração ao sair), com `dbus-launch` próprio (senão o
  `audacious`/`audtool` falam com o do usuário). ~40% das entradas
  apontam para arquivos que não existem mais.
- Som de teste sem tocar nas caixas e no ritmo real: sink nulo
  (`pactl load-module module-null-sink sink_name=kpbench`, descarregar
  depois com `pactl unload-module N`) e `PULSE_SINK=kpbench` (vale para o
  `kiplay-audio` pelo plugin ALSA e para o Audacious).
- Xephyr para o usuário ver: `DISPLAY=:0 Xephyr :78 -screen 1280x800 -ac
  -br &`. Sem gerenciador de janelas os menus não abrem com o clique do
  xdotool: use F10 e as setas.
- Mudar `VERSION` (ou outro `-D` de `config.mk`) não força recompilação:
  `make clean` antes de conferir.

## Estrutura

| Arquivo | Papel |
|---|---|
| `src/main.c` | opções, gettext, abre a janela |
| `src/log.c`, `src/util.c`, `src/ini.c`, `src/prefs.c` | vindos do kidisks: log, caminhos (`kp_pathf`), .ini editável no lugar, preferências (`~/.config/kiplay/kiplay.ini`) |
| `src/playlist.c` | uma playlist: m3u8 com `#PLAYLIST:`, `#EXTINF`, `#EXTART`/`#EXTALB`; PLS; strings num `GStringChunk` (a string do local identifica a entrada), `?N` de subfaixa mantido |
| `src/tagreader.c` | tags em segundo plano pelo `kiplay-audio --tags`: fila, janela de 8, tempo-limite, sobe e sai sozinho |
| `src/media.c`, `src/search.c` | tipo de arquivo pela extensão (motor, pastas); palavras do Ctrl+J |
| `src/plset.c` | todas as playlists: `<id>.m3u8` numa pasta, ordem como lista de ids |
| `src/i18n.h` | `_()`, `N_()`, domínio `kiplay` |
| `ui/app.h` | o que as partes da janela compartilham, posições do vídeo e dos controles |
| `ui/window.c` | janela, menus (os comandos mostram a tecla pelo `kp_shortcuts_attach`), teclas fixas (mídia, Alt+1..9), menu do clique direito, salvar ao fechar |
| `ui/layout.c` | monta vídeo + abas + controles; move a área de vídeo sem trocar o XID |
| `ui/playlists.c` | abas (a lista de cada uma só nasce quando aparece), adicionar (pastas numa thread), remover, fila, arrastar, colunas, importar/exportar, pedidos de tags |
| `ui/plmodel.c` | `GtkTreeModel` virtual sobre o `kp_playlist` (sem cópia; `GtkTreeDragSource` para o arrastar do GTK 2) |
| `ui/jump.c` | Ctrl+J: busca na playlist visível, toca ou enfileira |
| `ui/controls.c` | barra de controle: botões, barra de tempo (pula para o clique), volume |
| `data/` | `.desktop` e ícone (achado em `../data` quando roda do `build/`) |
| `po/` | `kiplay.pot`, `LINGUAS`, `POTFILES.in`, um `.po` por idioma |
| `tools/fill_po.py`, `tools/lsan.supp` | traduções em lote; vazamentos conhecidos do GTK/fontconfig |
| `tools/audpl2m3u8.py`, `tools/bench-compare.sh` | playlists do Audacious para teste; Kiplay × Audacious |
| `src/backend.h`, `src/mpv.c` | o processo que toca: mpv `--idle` pelo JSON IPC, eventos neutros (`kp_event`), JSON sempre no locale C |
| `src/lineio.c`, `src/child.c` | linhas com um processo sem bloquear (fila, escrita não bloqueante); processo iniciado e acompanhado |
| `src/backend.c`, `src/backend_impl.h` | `kp_backend` como tabela de funções (mpv, kiplay-audio) |
| `src/audioback.c` | o `kp_backend` do kiplay-audio (pipes, protocolo de texto) |
| `audio/` | o `kiplay-audio`: `main.c` (threads, gapless, pausa), `dec.c` (libav → 48 kHz pela FIFO, libgme, busca), `out.c` (ALSA; `null` com relógio), `cmd.c` (protocolo), `tags.c` (modo `--tags`). Sem GLib |
| `src/json.c` | caminhos `a.b[0]` no cJSON (vindo do kiremote); `vendor/cjson/` |
| `src/instance.c` | instância única: socket unix por pasta de configuração, repasse de arquivos |
| `ui/player.c` | o controlador: qual motor (extensão), o que toca, avanço, gapless, erros, quedas, mpv ocioso, área de vídeo, título |
| `src/filesettings.c` | opções por arquivo (posição, faixas, atrasos, proporção): um `.ini` por arquivo, chave SHA-1 do local + tamanho |
| `src/mpvopts.c` | preferências → opções do mpv; a "assinatura" que diz quando reiniciá-lo |
| `src/mpris.c` | MPRIS 2 pelo GDBus (sem GTK): estado, metadados, pedidos do barramento por callbacks |
| `ui/shortcuts.c` | os comandos de teclado (tabela, padrões, `[shortcuts]`), o despacho das teclas e o editor |
| `ui/tracks.c` | menus Vídeo, Áudio e Legendas: faixas do arquivo, atrasos, proporção, mudo |
| `ui/prefsdlg.c` | Opções → Preferências: controles ligados a chaves do ini, aplicar ao dar OK |
| `ui/desktop.c` | MPRIS ligado ao player; ícone na bandeja |
| `src/ytsub.c` | modelo neutro das legendas do YouTube (pens, posições, estilos, eventos) e os padrões |
| `src/srv3.c`, `src/json3.c` | leitores de srv3 (GMarkup) e json3 (cJSON) |
| `src/sidecar.c` | arquivos ao lado do vídeo (legendas, chat), conversão para ASS em `~/.cache/kiplay/subs` |
| `src/ytsub_ass.c` | kp_ytsub → ASS: `\an`+`\pos`, cores, bordas; caixa (layer 0) e texto (layer 1) |
| `proto/subconv.c` | srv3 → ASS na linha de comando |
| `proto/embed.c` | mpv `--wid` numa `GtkDrawingArea`, IPC pelo GIO |
| `proto/audio.c` | libavformat/dr_mp3 → ALSA, para medir |
| `docs/FORMATOS.md` | srv3, json3, live_chat.json, audpl como observados |

`src/` (menos `main.c`) vira `build/libkp.a`, sem GTK (GLib sim), que é o
que os testes linkam. Código que precisa do GTK vai em `ui/`.

## Convenções

- Prefixo `kp_`/`KP_` em tudo que é público; um módulo por arquivo,
  cabeçalho com comentário explicando o porquê e de quem é a memória.
- Nada bloqueia a interface; quem decodifica roda em outro processo.
- `README.md` (pt-BR, padrão) e `README.en.md` andam juntos: mudou um, mude
  o outro.
- Licença GPL-3.0-or-later; `SPDX-License-Identifier` no topo de cada
  arquivo.
- Commits em português, terminando com a linha Co-Authored-By.
