# Formatos observados

O que o Kiplay lê e não tem especificação pública, como apareceu nos
arquivos reais de `testes/` (fora do repositório). Os testes de unidade usam
fixtures sintéticas no mesmo formato. Mudou o leitor? Atualize aqui.

Arquivos de teste (baixados com o yt-dlp 2026.08.19):

| Vídeo | Arquivos |
|---|---|
| かめりあ feat. ななひら, "ベースラインやってる？笑" (`D00hlkW0u3U`) | webm; vtt, srv3 e json3 em en, es-419, id, it, ko |
| Aki Rosenthal, 3DMV "シャルイース" (`Ii7rtNaGlls`) | mp4; vtt, srv3 e json3 em en, ja; `live_chat.json` |

Para baixar as legendas certas de um vídeo já baixado:

```sh
yt-dlp --skip-download --write-subs --sub-langs "all,-live_chat" \
       --sub-format srv3 URL          # ou json3
```

## srv3 (`<timedtext format="3">`)

```xml
<?xml version="1.0" encoding="utf-8" ?><timedtext format="3">
<head>
<pen id="3" b="1" fc="#EF3F72" fo="254" bo="0" ec="#88DCBC" sz="500" et="3"/>
<ws id="1" ju="2" pd="0" sd="0"/>
<wp id="86" ap="7" ah="11" av="28"/>
</head>
<body>
<p t="36340" d="100" wp="6" ws="1"><s p="4">texto </s><s>&#8203;</s><s p="8">PA</s><s p="4">RTY</s></p>
</body>
</timedtext>
```

- `<pen>`: `fc`/`fo` cor e opacidade do texto, `bc`/`bo` do fundo, `ec`
  cor da borda, `et` tipo de borda (1 sombra dura, 2 relevo, 3 contorno, 4
  sombra suave), `sz` tamanho em %, `fs` família (0 padrão, 1 Courier New,
  2 Times New Roman, 3 Lucida Console, 4 Roboto, 5 Comic Sans, 6 Monotype
  Corsiva, 7 versalete), `b`/`i`/`u`, `of` (0 subscrito, 1 normal, 2
  sobrescrito), `rb` (ruby). Opacidade vai até 254 (= opaco).
- Atributo ausente = padrão do YouTube: texto branco opaco, fundo `#080808`
  a 191 (75%), sem borda, `sz` 100.
- `<wp>`: `ap` 0–8 (ponto de ancoragem, em ordem de leitura: 0 topo-esq.,
  7 base-centro), `ah`/`av` em % do vídeo. A área útil é o vídeo menos 2%
  de cada lado: `x = 2% + ah × 0,96`. Ausente = `ap 7, ah 50, av 100`.
- `<ws>`: `ju` (0 esq., 1 dir., 2 centro), `pd` (direção; 2/3 vertical),
  `sd`.
- `<p>`: `t` e `d` em ms, `wp`, `ws`, `p` (pen). Texto direto no `<p>` usa
  o pen do `<p>`; `<s>` sem `p` também. `\n` literal é quebra de linha.
  `&#8203;` (espaço de largura zero) separa segmentos.
- Tamanho: o YouTube escala por um quarto da diferença, `1 + (sz − 100)/400`
  (`sz="2100"` = 6×). Os arquivos confirmam: em escala linear o "YEAH!!" de
  `sz="2100"` ficaria maior que a tela.
- Legendas automáticas (ASR) usam `<s t="..." ac="...">` com tempo por
  palavra: ainda não tratado.

### O que tem nos arquivos de teste

- **ko** (o mais "dançante"): 29 pens, 228 `wp`, 2 `ws`, 442 eventos de
  30 ms a 78 s (mediana 480 ms), até 20 eventos no mesmo segundo, `sz` até
  2100, contornos coloridos (`et="3"`), caixas coloridas (`bc="#00B7FF"`),
  um painel cinza feito com um pen de `sz="1660"` e fundo `#B0B0B0`. O
  efeito "PA→PAR→PARTY" é uma sequência de eventos de 100–300 ms trocando o
  pen de cada pedaço.
- **Akirose ja**: 18 pens que só mudam `fo` (de 254 a 90), todos com
  `fs="6"` (Monotype Corsiva) e `et="4"`: transição de opacidade em degraus.
- O vtt dos mesmos vídeos não tem posição, cor nem tamanho.

### Fontes nesta máquina

`fc-match` acha Roboto e as metric-compatible (Cousine, Tinos, Arimo), mas
Monotype Corsiva e Comic Sans caem em fontes sem relação. O conversor usa
`Z003` (URW Chancery) para `fs=6`. Configurável depois.

### Tamanho base

`em_ratio = 0.05` (fonte ASS a 5% da altura com `sz=100`) é um chute
inicial. **Falta calibrar** comparando capturas do player do YouTube com as
do mpv no mesmo instante.

## json3

Mesmo conteúdo do srv3 em JSON (`pens`, `wsWinStyles`, `wpWinPositions`,
`events` com `tStartMs`, `dDurationMs`, `segs[].utf8`, `segs[].pPenId`).
Leitor a fazer na fase 5.

## live_chat.json (yt-dlp)

Uma ação por linha:

```json
{"replayChatItemAction":{"actions":[{"addChatItemAction":{"item":{
  "liveChatTextMessageRenderer":{"message":{"runs":[{"text":"..."},
  {"emoji":{"emojiId":"🍎","shortcuts":[":red_apple:"],"image":{"thumbnails":[{"url":"..."}]}}}]},
  "authorName":{"simpleText":"..."},"authorExternalChannelId":"UC...",
  "authorPhoto":{...},"timestampUsec":"...","id":"..."}}}}],
  "videoOffsetTimeMsec":"123456"},"clickTrackingParams":"..."}
```

Akirose (estreia de 7,5 min, 1468 linhas, 2,2 MB):

| Item | Quantos |
|---|---|
| `liveChatTextMessageRenderer` | 1391 |
| `liveChatPlaceholderItemRenderer` | 61 |
| `liveChatPaidMessageRenderer` (Super Chat) | 10 |
| `addLiveChatTickerItemAction` | 5 |
| `liveChatViewerEngagementMessageRenderer` | 1 |

- 52 linhas com `videoOffsetTimeMsec` = 0: o chat da sala de espera, antes
  da estreia.
- Emojis Unicode vêm com o caractere em `emojiId` (desenhar com a fonte
  local); emotes de canal têm `isCustomEmoji` e só a URL da imagem.

## Playlists do Audacious (`~/.config/audacious/playlists/*.audpl`)

Só para os testes de comparação (o Kiplay não importa playlists do
Audacious). Texto, um campo por linha, valores com escape de URL; cada
entrada começa em `uri=` e é seguida de `title=`, `artist=`, `album=`,
`length=` (ms), `codec=` etc. A ordem das playlists está em
`~/.config/audacious/playlist-order`.

- Arquivos com várias músicas (NSF, SPC, GBS...) levam o número da faixa
  na URI: `file:///.../jogo.nsf?1`.
- Nesta máquina: 21 playlists, 27.689 entradas. Formatos: MP3 ~14.400,
  Ogg Vorbis 7.393, AAC 1.176, NES/Famicom ~1.400, Super Nintendo 897,
  Sega (Genesis, Master System, CD) ~330, MIDI 273, Game Boy 251, FLAC 98,
  WMA 35, Opus 22, WAV 13.
