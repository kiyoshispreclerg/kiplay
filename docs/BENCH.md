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
| Parado | **10,5 MB**, 0,00% | 52,1 MB, 0,07% | 52,1 MB, 0,10% |
| MP3 | **18,2 MB**, 0,86% | 48,0 MB, 1,23% | 47,8 MB, 0,47% |
| Ogg Vorbis | **16,9 MB**, 0,97% | 44,0 MB, 1,56% | 43,8 MB, 0,73% |
| AAC (m4a) | **23,4 MB**, 0,90% | 53,8 MB, 1,27% | 53,6 MB, 0,53% |
| FLAC | **18,1 MB**, 0,86% | 48,9 MB, 1,30% | 48,8 MB, 0,57% |
| Opus | **17,6 MB**, 1,27% | 50,9 MB, 1,60% | 50,8 MB, 0,90% |
| WMA | **18,9 MB**, 0,73% | 49,4 MB, 1,27% | 49,3 MB, 0,47% |
| WAV | **17,6 MB**, 0,74% | 48,0 MB, 1,27% | 47,9 MB, 0,47% |
| Game Boy (gbs) | **17,9 MB**, 1,23% | 44,1 MB, 1,23% | 44,0 MB, 0,60% |
| Mega Drive (vgz) | **18,2 MB**, 13,8% | 44,8 MB, 1,87% | 44,6 MB, 1,10% |

Do Kiplay tocando, ~12 MB são a janela e ~6 MB o `kiplay-audio`.

## O que se vê

- **Memória**: o Kiplay usa 2,5 a 5 vezes menos, e abre em um terço do
  tempo. Antes da lista virtual (fase 3) ele mesmo usava 22,5 MB e levava
  690 ms com essas playlists.
- **CPU**: abaixo do Audacious como o usuário o usa; o espectro custa ao
  Audacious ~0,7 ponto. Sem ele, o Audacious gasta menos que o Kiplay
  (0,5% × 0,9%): há o que ganhar no Kiplay (o motor acorda a cada 21 ms,
  o tempo vai 4×/s para a janela, que redesenha a barra).
- **Mega Drive**: custo da libgme 0.6.3 do sistema (o mpv gasta 28% no
  mesmo arquivo); o Audacious traz o próprio emulador, mais leve e menos
  preciso.

## Não comparável ou pendente

- **NES, SNES**: as faixas sorteadas eram vinhetas de poucos segundos.
  Na primeira rodada o Kiplay parava depois delas (subfaixa aberta de
  fora era acrescentada no fim da playlist; corrigido em `45da8d1`); na
  rodada refeita o NES ainda deu 0% de CPU no Kiplay: investigar antes
  de medir de novo, com faixas longas.
- **MIDI**: o Kiplay ainda não toca MIDI (vai para o mpv, que falha, e
  pula). O Audacious com o sintetizador usa ~200 MB.
- **Vídeo (mp4)**: o Kiplay mostra o vídeo pelo mpv (~118 MB, quase tudo
  do mpv); o Audacious toca só o áudio (126 MB).
- Faltam: tempo para abrir com a lista sem tags (`--no-tags`) e o pico
  do leitor de tags lendo uma playlist inteira.
