# Kiplay — plano

Player de áudio e vídeo para o KiDesktop: as playlists em abas do Audacious
com a configuração ampla do SMPlayer, em C11 + GTK2, o mais leve possível em
RAM e CPU. Quem decodifica e desenha o vídeo é um programa externo plugável
(mpv primeiro, MPlayer depois), como no SMPlayer. A música toca num motor de
áudio próprio e pequeno, o `kiplay-audio`, também num processo à parte.

O que nenhum dos dois faz e o Kiplay vai fazer: mostrar, o mais perto
possível do original, o que o yt-dlp baixa junto do vídeo:

- as legendas do YouTube **com estilo e posição** (srv3/json3: cores,
  contorno, posição na tela, texto vertical, as "legendas dançantes").
  Vídeo de teste: o clipe de "Bassline Yatteru";
- o **chat de live gravado** (`.live_chat.json`) em dois modos: subindo ao
  lado do vídeo, como no YouTube, ou atravessando a tela (danmaku/bullet,
  como no Niconico e no Bilibili);
- as legendas comuns que o SMPlayer já lê (srt, ass, vtt, sub, idx...).

## Objetivos

- Várias playlists em paralelo, em abas, cada uma com sua posição; a aba que
  toca não precisa ser a aba que está à mostra (como no Audacious).
- Áudio e vídeo na mesma janela, com layout configurável (ver "Layout").
- Backend de vídeo externo trocável (mpv, MPlayer), escolhido na
  configuração; música pelo `kiplay-audio` (ou pelo backend de vídeo, se o
  usuário preferir).
- Nada que um arquivo ruim faça trava ou derruba a janela: quem decodifica
  sempre roda em outro processo.
- Configuração no nível do SMPlayer: preferências em abas, opções por arquivo
  (posição, faixas, atrasos), atalhos editáveis, opções extras repassadas ao
  backend.
- Achar sozinho, na pasta do vídeo, as legendas e o chat que vieram junto.
- Medir e manter: Kiplay parado com uma playlist de 10 mil faixas abaixo de
  ~15 MB de memória própria (`Private_Dirty`, como no kidisks); CPU perto de
  zero enquanto toca (o trabalho pesado é do backend).

Fora do escopo por enquanto: Wayland, Windows, skins do Winamp, visualizador
de espectro, edição de tags, gravação de TV/DVB.

## Arquitetura

```
kiplay                (C11, GTK2; laço do GLib, sem threads na interface)
 ├─ janela ─ área de vídeo (GtkDrawingArea → XID → --wid do backend)
 │         ├─ painel de chat (lado, opcional)
 │         ├─ barra de controle (tempo, volume, faixas; topo ou rodapé)
 │         └─ abas de playlists (GtkNotebook, uma lista virtual por aba)
 ├─ backend ─┬─ socket JSON IPC ──► mpv  (processo à parte, --idle)
 │           ├─ stdin/stdout -slave ──► mplayer
 │           └─ pipe + memória compartilhada ──► kiplay-audio (música)
 ├─ sidecars ─ acha legendas, chat, info.json, capa ao lado do arquivo
 ├─ conversores ─ srv3/json3 → ASS; live_chat.json → índice; chat → ASS danmaku
 └─ leitor de tags (thread de trabalho, cache em disco)
```

### Princípios

- O Kiplay é só interface e orquestração. Decodificar, desenhar vídeo e
  legenda é do backend, que roda em outro processo: se travar, o Kiplay
  continua de pé e reinicia o backend.
- Um processo do backend fica vivo em `--idle` e recebe os arquivos um a um
  (`loadfile`), em vez de abrir um processo por faixa: troca de faixa sem
  custo de inicialização e com gapless.
- Tudo o que chega do backend entra no laço do GLib (fd do socket num
  `GIOChannel`); nada bloqueia a interface. A única thread é a do leitor de
  tags (e a do indexador de chat, se a medição pedir), com resultados pelo
  `g_idle_add`.
- Nada de propriedade observada em alta frequência sem necessidade: a barra
  de tempo pede `time-pos` 4×/s por um timer (o `observe_property` do mpv
  manda o tempo a cada quadro: ~1,2% de CPU só para isso no protótipo); o
  painel de chat só redesenha quando entra mensagem.
- Legendas estilizadas e danmaku viram **ASS**, que o mpv e o MPlayer já
  desenham com a libass. Assim o Kiplay não desenha nada sobre o vídeo e
  não paga composição nem janela sobreposta.
- Listas grandes (playlists de milhares de faixas, chats de horas) são
  desenhadas como listas virtuais: só as linhas visíveis existem como texto
  formatado.

### Dependências

| Biblioteca | Uso |
|---|---|
| GTK2 (≥ 2.24), GLib/GIO | interface, laço, subprocessos (`GSubprocess`), GMarkup (srv3 é XML simples) |
| pango, cairo | painel de chat, medir largura do texto no danmaku |
| cJSON (embutido) | IPC do mpv, json3, live_chat.json, info.json |
| libavformat (decidir na fase 3) | ler tags e duração para a playlist, numa thread |
| libdbus-1 ou GDBus | MPRIS (servidor: o kikarinhas e o desktop leem a música tocando) |
| mpv (≥ 0.34), mplayer, yt-dlp | só ao rodar, programas externos |

Build com `Makefile` + `config.mk` + pkg-config, sem avisos (`-Werror
-Wpedantic`), como no kidisks e no kikarinhas.

## O backend plugável

Uma interface pequena em `src/backend.h`; cada backend é um arquivo:

```c
struct kp_backend_ops {
    const char *name;            /* "mpv", "mplayer" */
    unsigned caps;               /* KP_CAP_IPC_EVENTS, KP_CAP_GAPLESS, KP_CAP_SUB_ADD, ... */
    int  (*start)(kp_backend *b, unsigned long wid, const kp_opts *o);
    int  (*load)(kp_backend *b, const char *path, const kp_load_opts *lo);
    void (*command)(kp_backend *b, kp_cmd cmd, const kp_value *arg); /* pause, seek, volume, sub-add... */
    void (*stop)(kp_backend *b);
};
/* Eventos voltam por callback neutro: tempo, duração, fim, faixas, erro. */
```

- **mpv** (primeiro): `mpv --idle --input-ipc-server=<sock> --wid=<XID>
  --no-terminal --no-input-default-bindings` + as opções do usuário.
  Comandos e `observe_property` pelo JSON IPC. Teclado e mouse ficam com o
  Kiplay (atalhos configuráveis), não com o mpv.
- **MPlayer** (depois): `-slave -idle -quiet -wid`, comandos pelo stdin e
  respostas `ANS_*` pelo stdout; sem eventos, então sondagem leve do tempo.
  Recursos que ele não tem viram `caps` desligadas e somem da interface.
- Opções extras de linha de comando por backend, como no SMPlayer, e um
  campo "opções avançadas" repassado sem mexer.
- Faixas de áudio só: o mesmo processo, com `--force-window=no` quando o
  arquivo não tem vídeo. Gapless (`--gapless-audio`, `--prefetch-playlist`),
  ReplayGain (`--replaygain`) e equalizador (filtro `lavfi` do mpv) vêm de
  graça.

## Layout

O conteúdo da janela é montado a partir de quatro peças: vídeo, chat
lateral, controles e playlists. Configurável (Preferências → Interface e
menu Exibir):

- **Vídeo**: acima das abas, ao lado delas (esquerda ou direita) ou
  escondido. Nos `GtkPaned`, a divisão é lembrada.
- **Controles** (play/pausa, anterior/próxima, repetir, aleatório, barra de
  tempo, volume): no topo ou no rodapé da janela.
- **Só música** (nenhum vídeo na faixa atual nem na fila): a área de vídeo
  some e a playlist ocupa a janela toda. Configurável: "esconder o vídeo
  quando não houver" (padrão) ou "manter a área com a capa".
- **Tela cheia**: só o vídeo (e o danmaku, que é legenda); controles e chat
  lateral aparecem ao encostar o mouse, como no SMPlayer.
- Trocar de layout reorganiza os mesmos widgets (`gtk_container_remove` com
  referência + `gtk_box_pack`), sem recriar a área de vídeo: o XID não muda
  e o mpv não precisa reiniciar.

## Motor de áudio (`kiplay-audio`)

Medido nesta máquina com o clipe de "Bassline Yatteru" (webm/opus),
tocando só o áudio, memória própria (`Private_Dirty`):

| Programa | Memória própria | CPU |
|---|---|---|
| mpv 0.34 tocando | 39 MB (33 MB sem os scripts Lua) | ~1,6% |
| mpv 0.34 ocioso (`--idle`) | 22 MB | 0% |
| MPlayer 1.4 tocando | 11,5 MB | ~1% |

Para vídeo isso não importa (o vídeo pesa muito mais), mas para quem deixa a
música tocando o dia todo o mpv é caro. Decisão: um motor de áudio próprio,
**num processo à parte**, como o mpv:

- **Por que processo e não thread**: uma thread no Kiplay seria o mais leve,
  mas um decodificador que trava ou dá segfault (arquivo corrompido, bug de
  biblioteca) derrubaria a janela e as playlists junto. Num processo, o
  Kiplay vê o pipe fechar, avisa, pula a faixa e reinicia o motor. O custo
  extra de um processo pequeno em C é de poucas centenas de kB.
- **O mesmo contrato dos outros backends** (`kp_backend_ops`, com `caps`
  próprias): para o Kiplay é só mais um backend, escolhido por faixa. Faixa
  com vídeo → mpv/MPlayer; só áudio → `kiplay-audio` (configurável: dá para
  mandar tudo para o mpv).
- **Dentro dele**: uma thread de decodificação e outra de saída, ligadas por
  um buffer circular; comandos pelo pipe (texto, uma linha por comando,
  como o `-slave` do MPlayer); saída ALSA sem bloquear (vem do `audio.c` do
  kikarinhas, que já funciona com PipeWire/PulseAudio pelo plugin ALSA).
- **O que ganha em relação ao mpv**: gapless de verdade (a próxima faixa já
  decodificada no buffer), crossfade, ReplayGain, equalizador próprio
  (biquads, barato) e as amostras PCM numa memória compartilhada para um
  visualizador futuro, coisas que o mpv não entrega ao Kiplay.
- **Decodificadores: libavformat/libavcodec** (decidido na fase 0, pela
  medição abaixo). Custa ~1 MB a mais que um decodificador mínimo e abre
  tudo, inclusive as músicas de console pelo demuxer `libgme` do ffmpeg
  (NSF, SPC, GBS, VGM).
- **MIDI** (273 entradas no Audacious do usuário): libfluidsynth com um
  SoundFont configurável; o ffmpeg não toca MIDI.
- **Subfaixas**: arquivos de console têm várias músicas; a playlist guarda
  `arquivo.nsf?N` (a mesma notação do Audacious) e o motor escolhe a faixa
  (`track_index` do libgme).
- **Meta**: abaixo de 6 MB de memória própria e de 1% de CPU com formatos
  comprimidos. A emulação de console custa mais CPU, e isso é normal: o mpv
  gasta o mesmo.

Medido com o protótipo `proto/audio.c` (libavformat → ALSA, dispositivo
`null` com relógio), arquivos da biblioteca do usuário, memória própria:

| | Memória própria | CPU |
|---|---|---|
| protótipo, MP3 / Ogg / AAC / FLAC | 6,0 / 6,0 / 6,9 / 5,6 MB | 0,2–0,6% |
| protótipo, NES (NSF) / SNES (SPC) | 5,6 / 5,6 MB | 1,6% / 2,8% |
| protótipo com dr_mp3 em vez da libavcodec, MP3 | 5,0 MB | 0,2% |
| MPlayer 1.4, MP3 | 7,8 MB | 0,8% |
| mpv 0.34, MP3 / SPC | 22,8 / 23,0 MB | 1,2% / 3,0% |

Boa parte dos ~5 MB é a pilha ALSA → plugin do PulseAudio/PipeWire, que
qualquer programa paga.

## Playlists (o lado Audacious)

- Abas com nome; criar, renomear, fechar, reordenar arrastando; cada uma
  guardada em `~/.config/kiplay/playlists/NN.m3u8` (gravação atômica).
- Colunas configuráveis (n.º, título, artista, álbum, duração, caminho);
  lista virtual (`GtkTreeView` com modelo próprio, sem `GtkListStore` de
  10 mil linhas, se a medição pedir).
- Tags lidas numa thread, com cache por caminho + mtime em
  `~/.cache/kiplay/tags`; a lista aparece na hora e preenche depois.
- Fila (Q), aleatório, repetir (faixa, playlist), parar após a faixa,
  "pular para arquivo" com busca (Ctrl+J), arrastar arquivos e pastas,
  importar e exportar m3u/m3u8/pls.
- Aba tocando ≠ aba visível; a aba que toca ganha um marcador.

## Configuração (o lado SMPlayer)

- `~/.config/kiplay/kiplay.ini`, editável à mão (o `ini.c` do kikarinhas
  mantém comentários).
- Preferências em abas: Geral, Vídeo, Áudio, Legendas, Chat, Playlists,
  Atalhos, Backend (programa, caminho, opções extras), Avançado.
- Opções por arquivo (como o `file_settings` do SMPlayer): posição para
  retomar, faixas de áudio e legenda, atrasos, volume, proporção. Chave =
  hash do caminho + tamanho, em `~/.local/share/kiplay/files/`.
- Atalhos editáveis; MPRIS; ícone na bandeja opcional; modo compacto.

## Arquivos ao lado do vídeo (sidecars)

O padrão suportado é o nome padrão do yt-dlp, `Titulo [ID].ext`: o `[ID]`
do fim dá o id do vídeo (usado para o cache e, depois, para buscar o que
faltar). A partir de `Titulo [ID].mp4`, o yt-dlp grava:

| Arquivo | O que é | O que o Kiplay faz |
|---|---|---|
| `Titulo [ID].<lang>.srv3` / `.json3` | legenda do YouTube com estilo | converte para ASS e entrega ao backend |
| `Titulo [ID].<lang>.vtt` / `.srt` / `.ttml` | legenda comum (o estilo do YouTube se perde) | entrega como está |
| `Titulo [ID].live_chat.json` | chat da live, uma ação por linha | painel lateral e/ou danmaku |
| `Titulo [ID].info.json` | título, canal, capítulos, descrição, comentários (`--write-comments`) | metadados, capítulos, aba de comentários (depois) |
| `Titulo [ID].webp` / `.jpg` | miniatura | capa no modo áudio |
| `*.ass`, `*.ssa`, `*.sub`, `*.idx`, `*.smi`... | legendas comuns | entrega como está (o que o SMPlayer já faria) |

Casamento pelo nome-base exato (`Titulo [ID]` + `.<lang>.<ext>`), com o
idioma tirado do sufixo. Isso também pega arquivos com outro nome, desde que
vídeo e legendas tenham o mesmo nome-base; sem `[ID]`, só não há id. Tudo aparece no menu Legendas,
agrupado: "YouTube (estilo original)", "Comuns", "Chat". Quando há srv3 e
vtt do mesmo idioma, o srv3 ganha.

Recomendação para o usuário (vai para o README; o yt-dlp desta máquina
está em `~/Downloads/yt-dlp_linux`):
`yt-dlp --write-subs --sub-langs all,live_chat --sub-format srv3/json3/best
--write-info-json --write-thumbnail URL`. O formato padrão do yt-dlp é vtt,
que **perde** posição e estilo: sem srv3/json3 não há legenda dançante.

## Legendas do YouTube (srv3/json3 → ASS)

O srv3 (`<timedtext format="3">`) e o json3 (mesmo conteúdo em JSON) têm:

- **pens** (`<pen>`): negrito/itálico/sublinhado, cor e opacidade do texto
  (`fc`/`fo`) e do fundo (`bc`/`bo`), contorno (`ec`/`et`: sombra, relevo,
  brilho/contorno, sombra suave), fonte (`fs`, 7 famílias), tamanho (`sz`),
  sobrescrito/subscrito (`of`), ruby/furigana (`rb`);
- **window positions** (`<wp>`): ponto de ancoragem 0–8 e posição em % da
  tela (`ap`/`ah`/`av`);
- **window styles** (`<ws>`): justificação e direção (texto vertical,
  comum em clipes japoneses);
- eventos `<p t d wp ws p>` com segmentos `<s>` que trocam de pen no meio
  da linha.

A "dança" não é um efeito especial do formato: é feita com muitos eventos
curtos em posições e cores diferentes. O srv3 coreano do "Bassline
Yatteru" (`testes/`) mostra isso: 29 pens, 228 window positions, 442 eventos
de 30 ms a 78 s (mediana 480 ms), até 20 eventos no mesmo segundo, fontes
até `sz="2100"` (21× o normal) e contorno colorido (`et="3"` com `ec`
rosa/verde). O vtt do mesmo vídeo não tem nada disso. Por isso a conversão para ASS
(`\an`, `\pos`, `\c`, `\alpha`, `\bord`/`\shad`, `\fs`, `\fn`) consegue ser
fiel. Pontos de cuidado:

- **Tamanho e fonte**: o player do YouTube escala a fonte pela altura do
  vídeo e usa caixa de fundo preta a 75%. `PlayResX/Y` do ASS igual à
  proporção do vídeo e calibração por captura lado a lado (fase 0).
- **Ruby**: a libass não tem ruby; vira texto menor posicionado acima, como
  faz o YTSubConverter.
- **Vertical**: fonte com `@` (suporte parcial da libass); se ficar ruim,
  empilhar caractere por caractere.
- Referência para conferir semântica (não copiar código sem checar a
  licença): YTSubConverter (arcusmaximus), que converte ASS ↔ srv3.

Calibrado contra uma gravação do player do YouTube (ver
`docs/FORMATOS.md`): tamanho e posições batem. Opções para o usuário:

- **Contorno**: "como no YouTube" (linha fina, padrão) ou "brilho forte"
  (contorno grosso e borrado, mais legível; `edge_scale` do conversor).
- **Fonte para U+3164**: o desenho de caixas com `ㅤ` depende de a fonte
  dar largura a esse caractere (ver `docs/FORMATOS.md`).
- **Mover as legendas com o mouse** (depois): arrastar uma legenda sobre o
  vídeo, como no YouTube, mas o deslocamento vale para **todas** as
  legendas daquele vídeo e fica salvo nas opções do arquivo. Viável porque
  o Kiplay conhece a posição de cada evento: o clique cai na área de vídeo
  do Kiplay (o mpv roda com `--input-cursor=no`), o Kiplay acha qual
  evento está sob o cursor pelo modelo (`kp_ytsub` + medida do texto),
  e ao soltar regera o ASS com `(dx, dy)` somados a todos os `\pos` e
  manda `sub-reload` (442 eventos convertem em milissegundos). Durante o
  arraste, uma prévia leve pelo `osd-overlay` do mpv. Para legendas
  comuns (srt/vtt), o mesmo efeito com `sub-pos`/`sub-margin-x`.

O ASS gerado vai para `~/.cache/kiplay/subs/<hash>.ass` (refeito se o srv3
mudar) e entra no backend por `sub-add`. Também sai pela linha de comando:
`kiplay --convert X.srv3 > X.ass`, útil para teste e para quem quiser usar
em outro player.

## Chat de live gravado (`live_chat.json`)

Formato (JSON por linha, gravado pelo yt-dlp a partir do replay do chat):

```json
{"replayChatItemAction":{"actions":[{"addChatItemAction":{"item":{
  "liveChatTextMessageRenderer":{"message":{"runs":[...]},"authorName":{...},
  "authorExternalChannelId":"UC...","authorBadges":[...],"timestampUsec":"..."}}}}],
  "videoOffsetTimeMsec":"123456"}}
```

Observado no chat da Akirose (`testes/`, estreia de 7,5 min, 1468 linhas):
1391 mensagens de texto, 10 Super Chats, 61 placeholders, 5 itens do
ticker, 1 aviso de engajamento. As 52 linhas com `videoOffsetTimeMsec = 0`
são o chat da sala de espera, antes da estreia: o painel lateral mostra
essas como histórico já presente no início; o danmaku as ignora (ou espalha
nos primeiros segundos, configurável). Emojis vêm em `runs` com o caractere
em `emojiId` e uma URL de PNG do Noto Emoji: usar a fonte local, sem rede;
só emotes de canal (`isCustomEmoji`) precisam de imagem baixada.

São os mesmos renderers que o `src/youtube.c` do kikarinhas já entende
(texto, Super Chat, membros, emojis em `runs`): reaproveitar o parser e o
cache de emotes, não reescrever.

- **Índice**: o arquivo de uma live longa passa de 100 MB. Na abertura, uma
  passada lê só `videoOffsetTimeMsec` e o deslocamento de cada linha
  (`mmap` + varredura, sem cJSON), e guarda `(tempo, offset)` num vetor
  compacto, com cache em disco. A mensagem só é decodificada quando vai
  aparecer.
- **Painel lateral** (modo YouTube): `GtkHPaned` ao lado do vídeo,
  desenho próprio com Pango/Cairo (nome colorido por selo, selos, emojis da
  fonte, emotes da plataforma como imagem, Super Chat como cartão colorido),
  sobe conforme o `time-pos`; no seek, recomeça pela busca binária no
  índice. Largura, fonte e "mostrar só Super Chats/membros" configuráveis.
- **Danmaku** (modo bullet): o chat vira ASS em que cada
  mensagem atravessa a tela da direita para a esquerda (`\move`), em faixas
  sem colisão (como o danmaku2ass), com largura medida pelo Pango.
  Configuráveis: duração da travessia, faixas, opacidade, área usada da
  tela (só o terço de cima, por exemplo), tamanho, limite de densidade
  (descarta o excesso em vez de sobrepor). Emotes da plataforma viram o
  nome (`:nome:`) no ASS; imagens no danmaku ficam para depois (`overlay-add`
  do mpv, ver riscos).
- **Medido na fase 0**: um chat de 4 h a ~10 mensagens/s, com 10 faixas na
  metade de cima da tela, dá 95 mil eventos (49 mil descartados por falta
  de faixa) e 8,5 MB de ASS. Desenhar custa ~5,6% de um núcleo em 1080p30
  (libass pelo filtro do ffmpeg). Carregar o arquivo inteiro **custa 76 MB
  a mais no mpv**: inaceitável. Por isso o danmaku é entregue aos pedaços:
  - plano A: script Lua do Kiplay carregado no mpv (`--script`), que lê um
    arquivo compacto já com faixas calculadas e desenha só o que está na
    tela com `mp.create_osd_overlay`; convive com a legenda normal;
  - plano B (MPlayer, ou se o A pesar): blocos de ~2 min trocados com
    `sub-add`/`sub-remove`, cada bloco com os eventos que estão na tela
    durante ele. No mpv 0.34 a legenda secundária (`secondary-sid`) perde
    o estilo, então o bloco ocupa a legenda principal, juntando legenda e
    danmaku no mesmo ASS.
- Os dois modos podem ficar ligados juntos; cada um tem atalho.

## Tocar direto das plataformas (depois)

Dá para fazer, e o mpv já toca links do YouTube pelo yt-dlp. O Kiplay faria:
colar um link → `yt-dlp -J` num subprocesso (sem bloquear) → lista de
formatos e legendas → baixa só o srv3/json3 e, se for VOD de live, o chat
para o cache → manda o link para o mpv com o formato escolhido. Live ao vivo
de verdade: o conector InnerTube do kikarinhas entrega as mensagens em tempo
real para o painel e o danmaku. Fica nas últimas fases porque depende de
APIs que mudam sem aviso; os arquivos locais são o caso principal.

## Estrutura de pastas

```
src/          sem GTK (vira build/libkp.a para os testes): backend, mpv, mplayer,
              playlist, m3u, sidecar, srv3, json3, ass, ytchat, chatindex, danmaku,
              filesettings, ini, log, util
ui/           GTK2: window, video, controls, playlists, chatpane, prefs, shortcuts
audio/        kiplay-audio: protocolo do pipe, decodificadores, buffer, saída ALSA
vendor/       cjson/; dr_libs/dr_mp3.h (só para a comparação do proto/audio)
proto/        protótipos da fase 0 (subconv, embed, audio): saem quando virarem programa
data/         .desktop, ícone, kiplay.ini de exemplo
po/           gettext (pt_BR primeiro)
tests/unit/   test_*.c com fixtures sintéticas (srv3, json3, live_chat)
tools/        gen_fixtures.py, comparação de captura (YouTube × Kiplay)
docs/         FORMATOS.md (srv3/json3/live_chat como observados), TESTE-MANUAL.md
```

Prefixo `kp_` em tudo que é público; um módulo por arquivo, com comentário
no cabeçalho explicando o porquê e de quem é a memória. Comentários e
identificadores em inglês; interface pelo gettext com msgids em inglês.

## Fases

0. **Investigação** *(feita)*:
   - arquivos de teste em `testes/` (fora do repositório): "Bassline
     Yatteru" (webm, vtt/srv3/json3 em en, es-419, id, it, ko) e o 3DMV da
     Akirose (mp4, vtt/srv3/json3 em en e ja, `live_chat.json`); formatos
     registrados em `docs/FORMATOS.md`;
   - srv3 → ASS já em `src/` (`srv3.c`, `ytsub.c`, `ytsub_ass.c`, com
     testes) e `proto/subconv`: a legenda coreana sai posicionada, com os
     contornos, as caixas e os textos gigantes do original;
     tamanho e borda calibrados contra uma gravação do player do YouTube
     feita pelo usuário (`em_ratio` 0,0625, borda fina de largura fixa);
   - `proto/embed`: mpv com `--wid` numa `GtkDrawingArea` + IPC pelo
     `GSocketClient` funciona; o Kiplay mínimo fica em **3 MB** de memória
     própria. O mpv com vídeo só dá para medir no X de verdade (no Xvfb
     tudo vira software);
   - `proto/audio`: motor de áudio com libavformat, ~6 MB (tabela acima);
   - danmaku de 4 h: +76 MB se carregado inteiro (ver "Chat de live
     gravado").
   Lições: `g_strescape` não serve para JSON (escapa UTF-8 em octal); o
   `observe_property` de `time-pos` chega a cada quadro.
1. **Esqueleto**: Makefile, `config.mk`, log/ini/util vindos do kidisks,
   gettext, harness de testes, `make test/asan/lint/pot`; janela com abas de
   playlist vazias.
2. **Backend mpv**: tocar áudio e vídeo embutido, barra de tempo, volume,
   pausa, seek, tela cheia, troca de faixa, reinício se o mpv morrer;
   layouts (vídeo acima/ao lado/escondido, controles no topo/rodapé).
2b. **`kiplay-audio`**: motor de música com gapless, ReplayGain, troca
   automática de backend entre faixa de áudio e de vídeo, reinício se cair.
3. **Playlists**: abas, m3u8, tags em thread com cache, fila, aleatório,
   repetir, busca, arrastar e soltar, gapless. Decidir libavformat × leitor
   próprio de tags pela medição.
   **Teste de comparação com o Audacious** (aceite da fase): um script de
   teste (`tools/audpl2m3u8.py`, não é função do Kiplay) converte as 21
   playlists do Audacious do usuário (27.689 entradas, `?N` incluído) em
   playlists do Kiplay, num `XDG_CONFIG_HOME` de teste. Os dois tocam a
   mesma música da mesma playlist; comparar memória própria
   (`Private_Dirty` de todos os processos de cada um: Kiplay +
   `kiplay-audio` × Audacious) e CPU ao abrir, parado e tocando, e o tempo
   para abrir com todas as playlists. Resultados em `docs/BENCH.md`.
4. **Configuração**: preferências, opções por arquivo (retomar), atalhos,
   MPRIS, bandeja.
5. **Sidecars e legendas do YouTube**: detecção, menu de legendas, srv3 e
   json3 → ASS, `--convert`. Aceite: o clipe de "Bassline Yatteru" com as
   legendas dançando como no YouTube.
6. **Chat lateral**: índice, painel, seek, emotes.
7. **Danmaku**.
8. **Backend MPlayer**, provando que a interface é mesmo plugável.
9. **Direto das plataformas**: link → yt-dlp → mpv, legendas e chat no
   cache; chat ao vivo pelo código do kikarinhas.
10. **Extras**: danmaku do Bilibili (`.xml`) e do Niconico, chat da Twitch
    (JSON do TwitchDownloader / chat-downloader), comentários e capítulos do
    `info.json`, equalizador, imagens no danmaku.

## Riscos

- **Fidelidade das legendas**: o YouTube não documenta o srv3; o que vale é
  o que o player dele mostra. Mitigação: fixtures pequenas por recurso
  (pen, posição, ruby, vertical) e comparação de captura.
- **Danmaku grande**: um ASS com centenas de milhares de eventos pesa na
  libass. Mitigação: limite de densidade, geração em blocos, plano B em Lua.
- **Imagens sobre o vídeo**: ASS não tem imagem. `overlay-add` do mpv
  aceita só 64 camadas BGRA; emotes no danmaku ficam como texto até haver
  um jeito barato.
- **`--wid` e foco**: o mpv embutido pode roubar teclado e mouse; o SMPlayer
  resolve com `--input-vo-keyboard=no` e repassando os eventos. Conferir na
  fase 0.
- **yt-dlp e InnerTube mudam sem aviso**: tudo o que depende deles fica
  isolado e é opcional; os arquivos locais continuam funcionando.

## Decisões tomadas

- Layout configurável: vídeo acima, ao lado ou escondido (só música);
  controles no topo ou no rodapé.
- Música no `kiplay-audio`, processo à parte, com libavformat (+ libgme
  pelo ffmpeg, + fluidsynth para MIDI); vídeo no mpv/MPlayer.
- Sidecars pelo padrão `Titulo [ID].ext` do yt-dlp.
- Danmaku entregue aos pedaços (script Lua no mpv; blocos como reserva).

## Decisões em aberto

- Leitor de tags (fase 3, pela medição): libavformat ou leitor próprio de
  ID3/Vorbis/MP4.
