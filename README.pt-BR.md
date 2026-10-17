# Orca Zoukei

[English](README.md)

O Orca Zoukei é um fork do [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) focado em impressoras **Creality série K (CFS)**, **Flashforge AD5X (ZMOD / HelixScreen / lessWaste)** e **Bambu (AMS / BMCU)**, com atualizador incremental via releases do GitHub.

Baseado em [Jacob10383/OrcaSlicer](https://github.com/Jacob10383/OrcaSlicer) `main` mais [WaveOverhangs](https://github.com/dennisklappe/OrcaSlicer-WaveOverhangs).

## Download

Última release: https://github.com/opoeta/OrcaZoukei/releases (v2026.10.17, zip Windows x64).
Descompacte em qualquer pasta e execute `orca-slicer.exe`. Atualize em **Ajuda > Verificar atualizações** (download delta, SHA-256, reversão automática; veja [docs/zoukei/updates.md](docs/zoukei/updates.md)).

## Recursos

**Impressoras**
- Creality: agente nativo do CFS, painel próprio do dispositivo (câmera, ventoinhas, timelapse, gráfico de temperatura, chaves de detecção por IA) e diálogo de envio (pré-seleção de slots, inspeção da 1ª camada).
- Flashforge: agente ZMOD/stock, sincronização bidirecional do IFS (projeto <-> impressora), detecção do HelixScreen, início de impressão com lessWaste, painel com editor de slots do IFS.
- Bambu: selos e agrupamento de slots do AMS, detecção de BMCU (link/verificação de firmware, nova tentativa automática do erro 050x-409D na A1/A1 mini, sem indicador de umidade), editor de cor com cores oficiais.

**Filamento**
- Filamentos multicolor: Dual / Tri / Quad / Gradiente, com efeito na prévia.
- Integração com Spoolman (somente leitura, veja [docs/zoukei/spoolman.md](docs/zoukei/spoolman.md)).
- Mapeamento de slots salvo no 3MF; nomes AMS / CFS / IFS por marca.

**Fatiamento**
- Posicionador de costura do PrusaSlicer 2.9 (opção de algoritmo de costura), posição de costura "Assemble zgap".
- Padrão de superfície sub-topo, otimização de deslocamento da superfície superior, posicionamento automático da torre de limpeza (do Bambu Studio 2.8).
- Passagem a ferro Archimedean Chords, largura de linha de preenchimento da 1ª camada.
- WaveOverhangs, contorno em Z.

**Visual**
- Prévia realista do G-code e da vista Preparar, no estilo do PrusaSlicer 3.0 (chaves em Preferências > Gráficos).

**Outros**
- API remota MCP em 127.0.0.1 com token.
- `app_language` no host de plugins.
- Correções de tradução pt-BR (ex.: "Costura scarf").

## Compilar (Windows)

Visual Studio 2026 com carga de trabalho C++; num prompt de desenvolvedor: `build_win.bat`.

## Releases

`python tools/zoukei/make_release.py` gera o zip, o manifesto e os deltas. Processo e formato: [docs/zoukei/updates.md](docs/zoukei/updates.md).

## Créditos e licenças

Licenciado sob **AGPL-3.0** (veja `LICENSE.txt`). O OrcaSlicer pertence a seus autores (SoftFever e colaboradores), AGPL-3.0, derivado do Bambu Studio e do PrusaSlicer (Prusa Research), também AGPL-3.0.

- [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) / SoftFever; fork de [Jacob10383](https://github.com/Jacob10383/OrcaSlicer)
- [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) (Prusa Research): posicionador de costura, referência de renderização 3.0
- Bambu Studio: superfície sub-topo, torre de limpeza, passagem a ferro, largura de preenchimento da 1ª camada
- Creality Print: recursos do painel e do envio; o código foi usado só como referência, o painel do Zoukei é código original
- heydemoura/creality-orca-integration: plugin do CFS (AGPL-3.0)
- OrcaStudio (jarczakpawel): suporte a BMCU
- HelixScreen: só referência de protocolo (GPL-3.0, sem código copiado)
- ff-5mp-api: referência de protocolo Flashforge (ISC)
- [MaxEllis/OrcaSlicer](https://github.com/MaxEllis/OrcaSlicer): API remota MCP
- [dennisklappe/OrcaSlicer-WaveOverhangs](https://github.com/dennisklappe/OrcaSlicer-WaveOverhangs): WaveOverhangs

## Aviso

Sem afiliação com OrcaSlicer, Bambu Lab, Creality, Flashforge ou Prusa Research. Marcas pertencem a seus donos. Fornecido como está, sem garantia.
