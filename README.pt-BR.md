# Kiplay

[English](README.md) · **Português**

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

<p align="center">
  <img src="docs/img/bassline.png" width="49%" alt="Legendas do YouTube com estilo, animadas sobre o vídeo">
  <img src="docs/img/akirose-chat.png" width="49%" alt="Chat de live gravado ao lado do vídeo">
</p>
<p align="center">
  <img src="docs/img/playlists.png" width="80%" alt="Playlists em abas">
</p>

**Estado:** em desenvolvimento, fase 6: já é um player de uso diário
(vídeo pelo mpv, música pelo motor próprio `kiplay-audio` com gapless e
música de console NSF/SPC/GBS/VGM, inclusive o FM do Master System, com
os nomes e durações do `.m3u` ao lado, e MIDI com um SoundFont à escolha; playlists em abas com tags, fila,
busca, arrastar para reordenar, importar e exportar m3u/pls; layout
configurável; preferências em abas, atalhos editáveis, continuar de onde
parou, faixas de áudio e legenda, MPRIS, bandeja, modo compacto; legendas
do YouTube com estilo e posição, como no site; chat de live gravado em
painel ao lado do vídeo). O chat em danmaku ainda não existe. Veja o [plano](PLANO.md) e a [comparação com o
Audacious](docs/BENCH.md).

## Instalar

Baixe o `.deb` da sua distro na
[página de Releases](https://github.com/kiyoshispreclerg/kiplay/releases)
(há para Ubuntu 22.04 e 24.04, e Devuan daedalus e excalibur; amd64) e
instale com o `apt`, que traz as bibliotecas de que ele precisa, mais o
`mpv` e o `curl` (pacotes recomendados; vídeo precisa do mpv ou do MPlayer,
música toca sem eles):

```sh
sudo apt install ./kiplay_<versão>-1_<distro>_amd64.deb
```

Para baixar legendas do YouTube, aponte `[tools] ytdlp` para um binário do
yt-dlp (veja Uso). Para desinstalar: `sudo apt remove kiplay`.

## Uso

```sh
kiplay                      # abre a janela
kiplay video.mkv musica.mp3 # adiciona à playlist da tela e toca o primeiro
kiplay ~/Música/Album       # pastas entram inteiras, por nome
kiplay -e *.mp3             # só adiciona (enfileira)
kiplay -n ~/Música/Album    # numa playlist nova (aba nova)
```

Com o Kiplay aberto, `kiplay ARQUIVO` manda o arquivo para a janela que
já existe. Depois do `make install` ele aparece no "Abrir com" dos tipos
de áudio e vídeo, e no menu do gerenciador de arquivos aparecem "Adicionar
à playlist do Kiplay" (a playlist atual, sem tocar) e "Adicionar a nova
playlist do Kiplay", esta também para pastas.

| Tecla | Faz |
|---|---|
| Espaço, tecla de mídia | tocar / pausar |
| ← / → | volta / avança 5 s (Shift: 1 min) |
| Ctrl+← / Ctrl+→, < / > | faixa anterior / próxima |
| + / −, M | volume, mudo |
| F, F11, duplo clique no vídeo | tela cheia (Esc sai; a barra de controle aparece ao mexer o mouse perto da borda, embaixo ou no topo) |
| Ctrl+C | modo compacto: só o vídeo e os controles |
| Ctrl+L | mostrar/ocultar as playlists (também o botão na barra de controle) |
| # , J, V | próxima faixa de áudio, próxima legenda, mostrar/esconder legenda |
| Ctrl++ / Ctrl+−, X / Z | atraso do áudio, atraso da legenda (100 ms) |
| A | próxima proporção (4:3, 16:9...) |
| Enter, duplo clique na lista | toca a faixa |
| Del | remove da playlist |
| Q | põe na fila (ou tira); a fila toca antes do resto |
| Ctrl+X, Ctrl+Ins, Ctrl+V | recortar, copiar e colar arquivos na playlist (Shift+Del e Shift+Ins também) |
| Ctrl+J | ir para arquivo: busca na playlist da tela |
| Ctrl+F | painel de busca ao lado das playlists: faixas de todas as playlists; duplo clique toca e mostra a aba; Esc fecha |
| Ctrl+M, Ctrl+R, Ctrl+S | parar após esta faixa, repetir, aleatório |
| Alt+↑ / Alt+↓, arrastar | move as faixas selecionadas |
| Ctrl+O, Ctrl+Shift+O | adicionar arquivos, adicionar pasta |
| Ctrl+T, F2, Ctrl+W | nova playlist, propriedades (nome, tipo), fechar |
| Ctrl+P | preferências |
| Alt+1..9 | vai para a playlist |

Os atalhos se trocam em Opções → Preferências → Atalhos (até duas teclas
por comando). Clique direito no vídeo ou na barra de controle abre um menu
com o essencial (útil no modo compacto e na tela cheia), inclusive as faixas de áudio e legendas do vídeo. Clique direito numa faixa da playlist: tocar, pôr na fila, recortar/copiar/colar, remover e abrir a pasta do arquivo.

**Legendas do YouTube:** se o nome do vídeo termina com o ID entre colchetes
(`Título [dQw4w9WgXcQ].mp4`, como o yt-dlp grava), o menu Legendas e o clique
direito no vídeo oferecem "Baixar legendas do YouTube...": o Kiplay pergunta
ao yt-dlp se o vídeo existe e quais idiomas tem (as enviadas e a automática no
idioma do vídeo), baixa as escolhidas ao lado do arquivo (no formato com estilo)
e já as carrega. O caminho do yt-dlp fica em Opções → Preferências → Avançado
(`[tools] ytdlp`).

**Baixar com o yt-dlp:** Arquivo → Baixar... recebe um ou mais links
(separados por espaço ou linha), a pasta de destino e as opções comuns do
yt-dlp: qualidade do vídeo, só áudio (m4a, mp3, opus...), modelo do nome do
arquivo (há prontos para playlists e títulos longos; o padrão é
`Título [ID].ext`, que o download de legendas acima entende), legendas,
miniatura, itens da playlist (`1:3`, `1,2,5`) e argumentos extras. O comando
que vai rodar aparece conforme você escolhe. Enquanto roda, as opções ficam
travadas, uma barra de progresso acompanha a saída do yt-dlp (item N de M numa
playlist) e só o Cancelar funciona. As escolhas ficam em `[download]`; o
caminho do yt-dlp é o mesmo `[tools] ytdlp`.

O Kiplay lembra de cada vídeo onde ele parou, as faixas de áudio e
legenda escolhidas, os atrasos e a proporção (em
`~/.local/share/kiplay/files/`), e continua dali da próxima vez. Em
Preferências → Geral dá para estender a música ou desligar.

As colunas (número, posição na fila, artista, álbum, duração, caminho) ficam em Exibir →
Colunas da playlist. As tags são lidas em segundo plano, num processo à
parte, e guardadas na playlist.

O tipo de uma playlist (botão direito na aba → Propriedades, ou F2) é
"Mista" ou "Só áudio". Numa playlist só de áudio tudo toca no motor leve
de música, vídeos também (só o som: poucos MB de memória e quase nada de
CPU, mesmo num arquivo grande), e um vídeo aberto enquanto ela é a atual
vai para a próxima playlist mista (uma nova, se não houver).

O Kiplay aparece para a área de trabalho por MPRIS (teclas de mídia,
applets, o kikarinhas) e pode ter um ícone na bandeja.

A configuração fica em `~/.config/kiplay/kiplay.ini`, que a janela de
preferências escreve e que também pode ser editado à mão (comentários são
mantidos; só o que difere do padrão é gravado):

```ini
[mpv]
# o programa, ou o caminho completo
program = mpv
# true: lê também o seu ~/.config/mpv/mpv.conf
use_config = false
# opções extras, como no SMPlayer
extra = --deband

[tools]
# yt-dlp (binário do Linux) para baixar legendas do YouTube; vazio: procura no PATH
ytdlp = ~/Downloads/yt-dlp_linux

[video]
hwdec = auto-safe

[subtitles]
languages = pt-BR,pt,en
font_size = 48

[playback]
# auto: música no kiplay-audio (leve, gapless); mpv: tudo pelo mpv
engine = auto
# saída do kiplay-audio: default (PulseAudio/PipeWire, ou o ALSA se não
# houver servidor) ou um dispositivo ALSA (hw:0, null...)
device = default

[files]
# off, video ou all: continuar de onde parou
resume = video

[shortcuts]
# teclas no formato do GTK; vazio: nenhuma
next = <Control>Right,greater
mute =
```

(Comentários só em linha própria: depois do valor, o `#` faria parte
dele.)

Para baixar um vídeo com as legendas no formato certo:

```sh
yt-dlp --write-subs --sub-langs all,live_chat --sub-format srv3/json3/best URL
```

O formato padrão do yt-dlp é vtt, que perde posição, cores e tamanhos.

O Kiplay acha os arquivos ao lado do vídeo pelo nome (`Título [ID].mp4` →
`Título [ID].ko.srv3`, `.json3`, `.vtt`, `.srt`, `.ass`...). De cada
idioma, o srv3 (ou o json3) vence os comuns: ele é convertido para ASS
(em `~/.cache/kiplay/subs`) e entra no menu Legendas como "estilo do
YouTube". O idioma é escolhido pela lista de [subtitles] languages.
Para converter à mão, para usar em outro player:

```sh
kiplay --convert "Título [ID].ko.srv3" > ko.ass
```

O chat gravado (`Título [ID].live_chat.json`, que o yt-dlp salva com
`--sub-langs live_chat`) aparece num painel ao lado do vídeo, seguindo o
tempo do vídeo e refazendo-se a cada pulo: mensagens com os emojis do
canal e os avatares (baixados uma vez com o `curl` para
`~/.cache/kiplay/chat`, com botão para limpar o cache em Preferências → Chat), Super Chats e novos membros em cartões coloridos,
nome em dourado/azul/verde para o canal, moderadores e membros. Menu Exibir
→ Chat (tecla `c`; em tela cheia o próprio mpv desenha o chat à direita do vídeo); em Preferências → Chat: largura e filtro ("só Super
Chats e membros").

Em [subtitles], `youtube_edge = strong` troca o contorno fino do YouTube por
um grosso e borrado, mais fácil de ler; `auto = no` desliga a busca.

## Compilar

Precisa de GTK 2, GLib, ALSA, zlib, um compilador de C++ e das
bibliotecas do FFmpeg (libavformat, libavcodec, libswresample), com os
pacotes `-dev`. O libpulse (`libpulse-dev`) é opcional, mas recomendado: o
`make` o usa se achar os cabeçalhos; senão avisa e compila só com ALSA
(`make PULSE=0` força isso, `make PULSE=1` exige o PulseAudio). A música
de console usa uma cópia do [Game_Music_Emu](https://github.com/libgme/game-music-emu)
em `vendor/gme` (LGPL-2.1-or-later). O MIDI usa o
[TinySoundFont e o TinyMidiLoader](https://github.com/schellingb/TinySoundFont)
em `vendor/tsf` (MIT e zlib) e precisa de um SoundFont General MIDI (`.sf2`):
indique o arquivo em Opções → Preferências → Áudio; vazio, vale o primeiro
achado em `/usr/share/sounds/sf2` (o pequeno TimGM6mb antes dos outros). Ele
fica na memória, com o dobro do tamanho do arquivo, só enquanto um MIDI toca.
Não há reverb nem chorus.

```sh
make
make test
make run             # roda do build/, com as traduções
sudo make install    # em /usr/local
```

## Licença

GPL-3.0-or-later.
