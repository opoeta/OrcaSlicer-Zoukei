# Orca Zoukei

[Português](README.pt-BR.md)

Orca Zoukei is an [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) fork focused on **Creality K-series (CFS)**, **Flashforge AD5X (ZMOD / HelixScreen / lessWaste)** and **Bambu (AMS / BMCU)** printers, with an incremental updater that pulls from GitHub releases.

Based on [Jacob10383/OrcaSlicer](https://github.com/Jacob10383/OrcaSlicer) `main` plus [WaveOverhangs](https://github.com/dennisklappe/OrcaSlicer-WaveOverhangs).

## Download

Latest release: https://github.com/opoeta/OrcaSlicer-Zoukei/releases (v2026.10.17, Windows x64 zip).
Unzip anywhere and run `orca-slicer.exe`. Update with **Help > Check for updates** (delta download, SHA-256 checks, automatic rollback; see [docs/zoukei/updates.md](docs/zoukei/updates.md)).

## Features

**Printers**
- Creality: native CFS printer agent, own device panel (camera, fans, timelapse, temperature chart, AI detection toggles) and send dialog (slot preselection, first-layer inspection).
- Flashforge: ZMOD/stock agent, IFS two-way sync (project <-> printer), HelixScreen detection, lessWaste-aware print start, device panel with IFS slot editor.
- Bambu: AMS slot badges and grouping, BMCU detection (firmware link/version check, auto-retry of error 050x-409D on A1/A1 mini, no humidity indicator), colour editor with official colours.

**Filament**
- Multi-colour filaments: Dual / Tri / Quad / Gradient, with preview effect.
- Spoolman integration (read-only, see [docs/zoukei/spoolman.md](docs/zoukei/spoolman.md)).
- Slot mapping saved in the 3MF; AMS / CFS / IFS naming per printer brand.

**Slicing**
- PrusaSlicer 2.9 seam placer (Seam algorithm option), "Assemble zgap" seam position.
- Sub-top surface pattern, top-surface travel optimisation, automatic wipe tower placement (from Bambu Studio 2.8).
- Archimedean Chords ironing, initial-layer infill line width.
- WaveOverhangs, Z contouring.

**Visual**
- Realistic G-code preview and Prepare view after PrusaSlicer 3.0 (toggles in Preferences > Graphics).

**Other**
- MCP Remote API on 127.0.0.1 with token.
- Plugin host `app_language`.
- pt-BR translation fixes (e.g. "Costura scarf").

## Building from source (Windows)

Visual Studio 2026 with the C++ workload, then from a Developer prompt: `build_win.bat` (see its options). Upstream build notes: [OrcaSlicer wiki](https://github.com/OrcaSlicer/OrcaSlicer/wiki).

## Releases

`python tools/zoukei/make_release.py` builds the zip, manifest and deltas. Process and asset layout: [docs/zoukei/updates.md](docs/zoukei/updates.md).

## Credits and licenses

Licensed under **AGPL-3.0** (see `LICENSE.txt`). OrcaSlicer is copyright its authors (SoftFever and contributors), AGPL-3.0, itself derived from Bambu Studio and PrusaSlicer (Prusa Research), also AGPL-3.0.

- [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) / SoftFever; [Jacob10383](https://github.com/Jacob10383/OrcaSlicer) fork
- [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) (Prusa Research): seam placer, 3.0 rendering reference
- Bambu Studio: sub-top surface, wipe tower placement, ironing, initial-layer infill width
- Creality Print: panel and send dialog features; its code was used as reference only, the Zoukei panel is original code
- heydemoura/creality-orca-integration: CFS plugin (AGPL-3.0)
- OrcaStudio (jarczakpawel): BMCU support
- HelixScreen: protocol reference only (GPL-3.0, no code copied)
- ff-5mp-api: Flashforge protocol reference (ISC)
- [MaxEllis/OrcaSlicer](https://github.com/MaxEllis/OrcaSlicer): MCP Remote API
- [dennisklappe/OrcaSlicer-WaveOverhangs](https://github.com/dennisklappe/OrcaSlicer-WaveOverhangs): WaveOverhangs

## Disclaimer

Not affiliated with OrcaSlicer, Bambu Lab, Creality, Flashforge or Prusa Research. Trademarks belong to their owners. Provided as is, without warranty.
