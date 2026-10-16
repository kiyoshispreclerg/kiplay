# Kiplay × Audacious

Comparação da fase 3 (ver `PLANO.md`), em 13/10/2026, com as playlists
reais do usuário: 20 playlists, 27.689 entradas, copiadas do Audacious por
`tools/audpl2m3u8.py`. Medido com `tools/bench-compare.sh`:

- os dois no mesmo X (Xephyr, sem gerenciador de janelas), janela de
  1280×780, som num sink nulo do PulseAudio (`PULSE_SINK`: ritmo real,
  sem som);
- Audacious 4.2 (interface Qt, saída PulseAudio) numa cópia da
  configuração do usuário, de duas formas: como ele usa (com o pequeno
  analisador de espectro da janela principal) e com o espectro desligado
  (`[qtui] infoarea_show_vis=FALSE`);
- cada medida abre o programa do zero, toca a mesma entrada da mesma
  playlist, espera 8 s, lê a memória própria (`Private_Dirty`) do programa
  e dos filhos (no Kiplay: `kiplay-audio`, mpv) e mede a CPU nos 15 s
  seguintes. Média de 2 rodadas;
- "abrir": do início até a janela aparecer, com as 20 playlists.

| | Kiplay | Audacious | Audacious sem espectro |
|---|---|---|---|
| Abrir | **130 ms** | 375 ms | 380 ms |
| MP3 | **18,4 MB**, **0,43%** | 46,8 MB, 1,40% | 46,0 MB, 0,55% |
| Ogg Vorbis | **17,9 MB**, **0,57%** | 43,8 MB, 1,75% | 43,7 MB, 0,98% |
| AAC (m4a) | **19,5 MB**, **0,57%** | 50,1 MB, 1,45% | 49,8 MB, 0,60% |
| FLAC | **18,1 MB**, 0,70% | 48,2 MB, 1,48% | 48,1 MB, 0,62% |
| Opus | **21,0 MB**, 1,05% | 47,4 MB, 1,77% | 47,2 MB, 0,93% |
| NES (Werewolf, faixa 8) | **18,6 MB**, **0,53%** | 45,9 MB, 1,45% | 45,6 MB, 0,62% |
| SNES (Donkey Kong Country, faixa 1) | **18,5 MB**, 1,50% | 43,9 MB, 1,88% | 43,5 MB, 1,12% |
| Game Boy (Pokémon Yellow, faixa 2) | **18,8 MB**, **0,45%** | 44,2 MB, 1,35% | 44,3 MB, 0,50% |
| Mega Drive (Battletoads, tema) | **22,7 MB**, 1,12% | 48,6 MB, 2,15% | 48,5 MB, 1,27% |
| Master System FM (Out Run 3D, faixa 1) | **18,6 MB**, 2,73% | 45,2 MB, 2,08% | 45,2 MB, 1,38% |

Rodada de 15/10/2026 (2 rodadas cada, mesmas condições da anterior, com o
motor novo: PulseAudio direto, meio segundo de buffer escrito em dois
pedaços, taxa do arquivo sem reamostrar, GME embutido). "Abrir" não mudou
(130 ms). Do Kiplay tocando, ~12 MB são a janela e ~6 MB o `kiplay-audio`.

Só o motor (`kiplay-audio` sozinho, antes → depois), CPU em 15 s:

| | Antes | Depois |
|---|---|---|
| MP3 | 0,70%, 81 despertares/s | 0,30%, 13 despertares/s |
| NES | 0,73% | 0,26% |
| Game Boy | 0,86% | 0,20% |
| Mega Drive | **15,7%** (YM2612 Nuked do sistema) | **0,73%** |
| SNES | 1,46% | 1,20% (a emulação do SPC700 em si) |
| Master System FM | não tocava | 2,40% |

## O que se vê

- **Memória**: o Kiplay usa 2 a 2,5 vezes menos, e abre em um terço do
  tempo.
- **CPU**: empatado ou abaixo do Audacious mesmo sem o espectro dele em
  quase todos os casos (MP3, Ogg, AAC, NES, Game Boy e Mega Drive abaixo; FLAC e
  Opus dentro de ~0,1 ponto, um pouco acima). Em dois casos o Audacious
  sem espectro ainda gasta menos: SNES (1,12% × 1,50%) e Master System FM
  (1,38% × 2,73%). Na FM o Audacious não tem emulador do YM2413 (não deve
  estar fazendo o trabalho que o Kiplay faz); não conferi o som dele.
- **De onde veio o ganho do MP3** (0,86% → 0,43%): a decodificação é só
  ~0,15% de um núcleo; o resto eram os ~75 despertares por segundo das
  escritas pequenas e bloqueantes através do plugin ALSA do PulseAudio
  (cada um custa nós e o servidor). Agora são 13 por segundo, e o volume é
  aplicado pelo servidor.
- **Mega Drive**: era a libgme 0.6.3 do sistema (YM2612 Nuked, cada ciclo
  do chip); o GME embutido usa o do Gens, 21 vezes mais barato nesse
  arquivo.

## Não comparável ou pendente

- **MIDI**: o Kiplay ainda não toca MIDI (vai para o mpv, que falha, e
  pula). O Audacious com o sintetizador usa ~200 MB. Medido o fluidsynth
  (libfluidsynth 2.2.5, 44,1 kHz): ~1,5% de CPU em tempo real, 1,1 MB só
  para carregar a biblioteca, e a memória depende do SoundFont: +11,6 MB
  (TimGM6mb, 6 MB), +33–49 MB (FluidR3 GM, 148 MB, com carga sob demanda)
  ou +160 MB (sem ela).
- **Vídeo (mp4)**: o Kiplay mostra o vídeo pelo mpv (~118 MB, quase tudo
  do mpv); o Audacious toca só o áudio (126 MB).
- Faltam: tempo para abrir com a lista sem tags (`--no-tags`) e o pico
  do leitor de tags lendo uma playlist inteira.
