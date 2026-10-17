# Mistura de cores (automatic colour mixing)

Orca Zoukei reproduces the colours of a model with the filaments you actually have loaded. It uses the
physical filaments directly or makes **mixed filaments** from them: virtual slots printed as alternating
layers or sub-layers of 2-3 physical filaments. A review dialog lists every recipe before anything is
applied.

## How to use

- **Textured / coloured import** (OBJ+MTL per-material colours, OBJ vertex colours, OBJ/glTF/GLB/FBX
  textures): in the import dialog, click **Mix colors automatically** (*Misturar cores automaticamente*),
  next to the CMYW/RYBW one-click mix. Review, then **Apply**. The rows are mapped to the chosen filaments
  and mixes, and OK paints the model as usual. The **Reset** button undoes the mapping.
- **Project / 3MF**: in the sidebar, use the palette icon beside *Add Mixed Filament*, or the one in the
  *Mixed Filament* title row. You can also click the notification shown after opening a 3MF that uses
  more colours than the printer holds.
  1. Tick the filaments that are loaded in the printer.
  2. The other colours are reproduced from the loaded ones.
  3. On **Apply**, the paint and the object, part and layer-range filament assignments are remapped.
     **Ctrl+Z** restores the previous assignment.
  4. The mixed filaments that were created stay in the list, like a manual *Add Mixed Filament*.
- **Options** (remembered in `OrcaSlicer.conf`, section `color_mix`):
  - **Target colors**: k-means in OKLab, default 8, import only.
  - **Max. new mixes**: default 4, capped by free slots.
  - **Ratio step**: 1/2 to 1/10, default 1/4.
  - **3-filament mixes**: off by default.
  - **Only physical filaments**: no mixes are created.
  - **Method**: Automatic, Alternating layers or Sub-layers.
  - **Color model**: Pigment or Optical.
  - **TD per filament**: HueForge transmission distance, in mm.
- **Method**:
  - *Alternating layers* switches off the print option `enable_mixed_color_sublayer`.
  - *Sub-layers* switches it on.
  - *Automatic* picks sub-layers for toolchangers and multi-nozzle printers (Snapmaker U1: no purge
    per switch). It picks alternating layers for a single nozzle with AMS / CFS / IFS / MMU, where every
    switch purges.
  - The dialog warns about purge waste on single-nozzle printers.

## Engine (`src/libslic3r/ColorMixEngine.*`, std-only, unit tests in `tests/libslic3r/test_color_mix_engine.cpp`)

| Piece | What it does |
|---|---|
| Colour spaces | sRGB ↔ linear, OKLab (Ottosson), CIELab D65, CIEDE2000 (Sharma 2005 reference pairs pass to 1e-4) |
| Recipe | Physical indices + integer weights, for example `{2,1}`. It gives the layer pattern (`AAB`, spread evenly like FullSpectrum's sequence builder) and percentages (largest remainder) as Orca stores them in `filament_mixed_sublayer_ratios`. |
| Prediction | 1) The **measured blend** from `standard_color_recipes.json` (Bambu's 486 measured prints) when the recipe is in the table. 2) Otherwise the **Pigment** model: the degree-4 Mixbox-like polynomial shared by Bambu, FullSpectrum and Orca, accumulated pairwise in double precision. Each weight is scaled by the stripe opacity (FullSpectrum's TD weighting). FullSpectrum 0.9.14 endpoint anchoring is optional. 3) Or the **Optical** model: stripes averaged in linear light, and translucent stripes filter the mix behind them. |
| TD / opacity | Opacity = 1 − 10^(−t/TD), where a layer one TD thick transmits 10 % (the HueForge/NeotkoCM convention). Without a TD, it is estimated from the type or name (translucent, clear, natural, PETG …) and the colour alpha. |
| Clustering | Weighted k-means in OKLab, deterministic k-means++ seeding. Import rows are weighted by painted face count. |
| Solver | Candidates are physical filaments, existing fixed mixes (free) and pairs/triples on a 1/L grid. The distance is CIEDE2000. A mix must beat the nearest physical filament by 1.5 ΔE, and a triple must beat the best pair by 0.5 (FullSpectrum's match heuristics). The mix limit is met by a facility-location drop and swap local search. The physical-only baseline is reported. |

### Quality numbers (`ColorMixEngine` against data, built standalone with MSVC)

- **Prediction against Bambu's 486 measured PLA Basic blends** (CMYW/RYBW, 2-3 components), mean ΔE00:

  | Model | Nominal filament colours | Each base colour fitted to the data |
  |---|---|---|
  | **Pigment** | **13.4** | **6.0** |
  | Pigment with endpoint anchoring | 13.8 | 6.9 |
  | Optical, opaque | 24.3 | 12.9 |
  | Optical, fully translucent | 16.6 | 9.4 |
  | Plain sRGB average | 15.6 | — |

  The prints are much duller than the nominal swatches. The measured table is used directly whenever it
  covers a recipe.
- **Solver, 24-colour hue wheel** (S 0.65, V 0.85), mean ΔE00 (worst in brackets). With CMYW loaded, the
  closest physical filament alone gives 19.1:

  | Settings | Mean ΔE00 |
  |---|---|
  | Pairs, 1/4 steps, ≤ 4 mixes | 10.2 |
  | Pairs, 1/4 steps, ≤ 8 mixes | 8.3 |
  | Pairs, 1/4 steps, unlimited | 7.9 (16.0) |
  | Pairs, 1/8 steps, unlimited | 6.8 |
  | Triples, 1/8 steps, unlimited | 4.5 (6.8) |

  - RYBW with ≤ 8 mixes: 23.4 → 12.3.
  - Black/white/red/blue with ≤ 8 mixes: 27.0 → 20.6.
- **Test asset `tests/data/color_mix/stacked_colours.obj+mtl`** (8 colours), with CMYW and ≤ 4 mixes:
  23.6 → 12.5. The worst colours are brown and grey, which CMYW alone cannot make.

## Comparison of the references

| | Bambu Studio 2.8 (Decompose Color + mixed filament) | FullSpectrum (ratdoux, Snapmaker Orca fork, 0.9.14) | Creality Print 7.3 | OrcaFS-NeotkoCM (Neotko) | **Orca Zoukei** |
|---|---|---|---|---|---|
| Mixing method | Sub-layers per layer, or whole-layer alternation | Layer cycle (A-A-B), "simple" pair, Local-Z sub-layers, gradient bands; pointillism enum retired | Whole-layer cadence plus 1-D ordered dither over the layer index, and sub-layer stacking. No in-layer interleave (pointillism keys are stubs). | ColorStitch per fill line (Bresenham dither, stripes, typed patterns) on top, penultimate and bottom surfaces. Lines are bucketed per tool into coplanar sub-layers. "Sandwich" top surface of up to 3 passes, PathBlend Z+flow gradient. | Bambu-compatible layer and sub-layer path (existing `FilamentMixer`). The engine chooses between them. |
| Colour prediction | Measured table plus Mixbox-like polynomial (Justin Hayes, MIT) | Same polynomial (endpoint-anchored in 0.9.14) or spectral Kubelka-Munk with trained residual tables. TD weighting. | Same polynomial for display. Closed-source KM recipe solver. | Linear light. Beer-Lambert TD stacking, opacity-weighted side-by-side average. | Measured table → polynomial (TD-weighted) or optical stripe model; per-filament TD |
| Match / solver | Brute force pairs and triples, 5 % grid, 20-80 %, ΔE76 | Coarse 5 % → top-K → 1 % refine, triples among the 8 nearest, ΔE00, prefer a pair unless the triple wins by 0.5 | Closed (`cr_km_recipe`) | Inverse match in ΔE00 for surface stacks | Pairs/triples on the 1/L pattern grid, ΔE00, global mix limit with drop/swap search, reuses existing mixes, k-means of model colours |
| Gradients | Z gradient with PCHIP curve, per part | Gradient stops with solid widths, Local-Z | Z gradient with curve | PathBlend (in-surface) | Existing Bambu gradient. The automatic solver uses fixed mixes only. |
| Printers | Single nozzle with AMS | Snapmaker U1 toolchanger (no purge between tools) | K2 / CFS single nozzle, normal flush | U1 with its own NeoTower (~10 k lines) | Method picked from the printer; purge warning for single nozzle |
| 3MF data | `filament_is_mixed`, `filament_mixed_components`, `filament_mixed_sublayer_ratios`, `filament_mixed_gradient*` | `mixed_filament_definitions` (one string, `;` rows); virtual IDs after the physical ones | `mixed_filament_definitions`. It imports Bambu keys and converts them. | FullSpectrum keys plus per-triangle `paint_colormix` and profile JSON | Bambu keys, unchanged. Bambu projects open as-is. |

### What came from where

| Source | What we reimplemented |
|---|---|
| Bambu | The mixed-filament slots, config keys and slicing (already in the tree), the measured blend table and the polynomial mixer it ships. The idea of a palette-constrained decomposition. |
| FullSpectrum | Recipe = layer pattern with an evenly spread sequence. Match heuristics (ΔE00, triple must beat pair by 0.5, minimum gain over a direct filament). TD weighting of components. Optional endpoint anchoring from 0.9.14 (measured to be worse on Bambu's data, so off by default). Recent upstream commits were reviewed up to `cc152a8` (0.9.14, 2026-09-27): gradient stop widths, KM training from higher-order samples, preserved mixed assignments on 3MF import (0.9.13) and image-map perimeter modulation. Only the endpoint fix fits our layer model now. |
| Creality Print | Confirms that sub-layer stacking and whole-layer cadence are the practical methods. Dithered layer cadence and the KM idea are follow-ups. |
| NeotkoCM | The TD convention (10 % transmission at one TD, Beer-Lambert) and the opacity-weighted stripe average behind the optical model. |
| Ours | OKLab k-means, global mix-limit optimisation, the optical stripe model, method choice by printer, the review dialog, and the import and project hooks. |

## Follow-ups (not in this change)

1. **Per-line dither ("linhas intercaladas" / "pontilhado")**, ColorStitch-style, on top and solid surfaces.
   - The surface fill is split into single lines.
   - A Bresenham share runs over lanes projected on the fill-angle normal.
   - The lines are regrouped in one child collection per tool and dispatched with `WipingExtrusions::set_extruder_override`.
   - Touches `Fill/Fill.cpp`, `GCode/ToolOrdering.cpp` (`resolve_mixed_filaments` must list the components for those layers) and the mixed-slot fallback in `GCode.cpp`.
   - Needs 6-8 new keys. About 400-600 LOC. It needs G-code validation with the CLI.
2. **Top-surface "Sandwich"**: 2-3 passes per top layer, each with its own tool, angle and share of the layer height. It reuses our sub-layer stacking.
3. **Dithered layer cadence** (Creality "advanced dithering"): rotate each cycle by a step coprime with its length so whole-layer patterns do not band.
4. **Import FullSpectrum / Snapmaker / Creality mixed filaments**.
   - Parse `mixed_filament_definitions` rows (`a,b,enabled,custom,mix_b%,…,g<ids>,w<weights>,…`) into our `filament_is_mixed`, `filament_mixed_components` and `filament_mixed_sublayer_ratios`.
   - Append the slots in row order so the painted virtual IDs (physical count + row) stay valid.
   - Needs sample 3MFs to validate.
5. **Kubelka-Munk spectral model** with per-filament reflectance, plus gradient recipes for colour ramps along Z (PathBlend-like smooth gradients).
6. Neotko extras worth a look later: Neoweaving (Z interlock), NeoStitch. Simplify3D import and LibreMode are out of scope.

## Untested

- The GUI has not been run against a real model import in this change; only the build was checked.
- No print or G-code was checked for the method switch.
- The 3MF notification was not tried on a real many-colour 3MF.
