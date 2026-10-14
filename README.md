# Kiplay

**Português** · [English](README.en.md)

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

**Estado:** em desenvolvimento, fase 6: já é um player de uso diário
(vídeo pelo mpv, música pelo motor próprio `kiplay-audio` com gapless e
música de console NSF/SPC/GBS/VGM; playlists em abas com tags, fila,
busca, arrastar para reordenar, importar e exportar m3u/pls; layout
configurável; preferências em abas, atalhos editáveis, continuar de onde
parou, faixas de áudio e legenda, MPRIS, bandeja, modo compacto; legendas
do YouTube com estilo e posição, como no site; chat de live gravado em
painel ao lado do vídeo). O chat em danmaku ainda não existe. Veja o [plano](PLANO.md) e a [comparação com o
Audacious](docs/BENCH.md).

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
| F, F11, duplo clique no vídeo | tela cheia (Esc sai; a barra de controle aparece ao mexer o mouse, embaixo ou no topo) |
| Ctrl+C | modo compacto: só o vídeo e os controles |
| # , J, V | próxima faixa de áudio, próxima legenda, mostrar/esconder legenda |
| Ctrl++ / Ctrl+−, X / Z | atraso do áudio, atraso da legenda (100 ms) |
| A | próxima proporção (4:3, 16:9...) |
| Enter, duplo clique na lista | toca a faixa |
| Del | remove da playlist |
| Q | põe na fila (ou tira); a fila toca antes do resto |
| Ctrl+J | ir para arquivo: busca na playlist da tela |
| Ctrl+M, Ctrl+R, Ctrl+S | parar após esta faixa, repetir, aleatório |
| Alt+↑ / Alt+↓, arrastar | move as faixas selecionadas |
| Ctrl+O, Ctrl+Shift+O | adicionar arquivos, adicionar pasta |
| Ctrl+T, F2, Ctrl+W | nova playlist, renomear, fechar |
| Ctrl+P | preferências |
| Alt+1..9 | vai para a playlist |

Os atalhos se trocam em Opções → Preferências → Atalhos (até duas teclas
por comando). Clique direito no vídeo ou na barra de controle abre um menu
com o essencial (útil no modo compacto e na tela cheia).

O Kiplay lembra de cada vídeo onde ele parou, as faixas de áudio e
legenda escolhidas, os atrasos e a proporção (em
`~/.local/share/kiplay/files/`), e continua dali da próxima vez. Em
Preferências → Geral dá para estender a música ou desligar.

As colunas (número, artista, álbum, duração, caminho) ficam em Exibir →
Colunas da playlist. As tags são lidas em segundo plano, num processo à
parte, e guardadas na playlist.

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

[video]
hwdec = auto-safe

[subtitles]
languages = pt-BR,pt,en
font_size = 48

[playback]
# auto: música no kiplay-audio (leve, gapless); mpv: tudo pelo mpv
engine = auto
# dispositivo ALSA do kiplay-audio
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
`~/.cache/kiplay/chat`), Super Chats e novos membros em cartões coloridos,
nome em dourado/azul/verde para o canal, moderadores e membros. Menu Exibir
→ Chat (tecla `c`); em Preferências → Chat: largura e filtro ("só Super
Chats e membros").

Em [subtitles], `youtube_edge = strong` troca o contorno fino do YouTube por
um grosso e borrado, mais fácil de ler; `auto = no` desliga a busca.

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
