# orcaslicer-mcp-zoukei

MCP bridge for **Orca Zoukei**. It is the stock [`orcaslicer-mcp`](https://github.com/maxellis/orcaslicer-mcp)
server (Max Ellis, AGPL-3.0, pinned to 0.1.17) plus the tools below, which drive the Zoukei Remote API
(`src/slic3r/GUI/RemoteAPI/`). Every stock tool (load_model, set_config, slice, ...) keeps working unchanged:
the stock package is a dependency whose server object we extend, not a copy.

> The Remote API listens on `127.0.0.1:13130` only and every request needs the token. Enable it in
> OrcaSlicer: Preferences > Remote API (copy the token). Nothing here can reach a printer unless you turn on
> Preferences > "Allow printer control" (default OFF) **and** pass `confirm=true` (group G5).

## Install (the owner runs this)

```powershell
D:/orca/orcaslicer-mcp-venv/Scripts/python.exe -m pip install -e D:/orca/k2-wave-main/tools/orcaslicer-mcp-zoukei
```

Register it in Claude Code (user scope) instead of the stock server; the Zoukei server includes all stock tools:

```powershell
claude mcp remove orcaslicer
claude mcp add orcaslicer --scope user -e ORCA_API_TOKEN=<token from Preferences> -e ORCA_API_URL=http://127.0.0.1:13130 -- D:/orca/orcaslicer-mcp-venv/Scripts/orcaslicer-mcp-zoukei.exe
```

## Tools added

Filament numbers are the 1-based project slots shown in the sidebar (physical filaments first, mixed after).

### G0 - colour features

| Tool | Remote API | What it does |
| --- | --- | --- |
| `filament_list` | `GET /filaments` | slots with colour, multi-colour, preset, type, TD (mm), mix recipe and predicted colour |
| `filament_set` | `PUT /filaments/{slot}` | colour, multi-colour string, TD used by the engine |
| `mixed_filament_list` | `GET /mixed_filaments` | mixed slots, global mode (layers / sublayers), free slots |
| `mixed_filament_add` | `POST /mixed_filaments` | components + ratios or layer pattern (`AAB`), mode, gradient; reuses identical mixes |
| `mixed_filament_update` | `PUT /mixed_filaments/{slot}` | change recipe in place |
| `mixed_filament_remove` | `DELETE /mixed_filaments/{slot}` | delete (undoable) |
| `color_mix_solve` | `POST /color_mix/solve` | recipes + predicted colours + CIEDE2000 for target colours, project untouched |
| `color_mix_apply` | `POST /color_mix/apply` | apply a solve result: create mixes, repaint / remap (one undo step) |
| `model_colors` | `GET /model/colors` | pending coloured import palette and filaments in use |
| `import_colored_model` | `POST /model/import_colored` | load a coloured OBJ/GLB/3MF without the dialog and return its palette |
| `set_object_filament` | `PUT /objects/{id}/filament` | filament of an object or of one part |

Typical flow for a coloured model: `import_colored_model` -> `color_mix_solve(targets="pending", loaded=[1,2,3,4])` ->
`color_mix_apply(solve_id)` -> `slice_and_wait`.


### G1 - supports

| Tool | Remote API | What it does |
| --- | --- | --- |
| `support_volume_list` | `GET /objects/{id}/support_volumes` | support enforcer / blocker volumes of an object |
| `support_volume_add` | `POST /objects/{id}/support_volumes` | enforcer or blocker as box / cylinder / sphere (size + world position) or from an STL/OBJ/3MF file |
| `support_volume_remove` | `DELETE /objects/{id}/support_volumes` | one volume, or all (optionally one kind) |
| `support_paint` | `POST /objects/{id}/support_paint` | paint enforcers / blockers on the facets a rule selects (overhang angle, height band, direction, region, fill) |
| `support_paint_clear` | `POST /objects/{id}/support_paint/clear` | clear all, or only the facets a rule selects |
| `support_paint_summary` | `GET /objects/{id}/support_paint` | painted state: facet counts per model part |

Per-object support settings (type, threshold angle, ...) are the stock `set_object_config`.

### G2 - project and model operations

Destructive project operations (`project_new`, `project_open`) are refused with 409 `unsaved_changes` unless
`discard_changes=true`. File writes go only to the path you pass, must carry the right extension, need an existing
folder, and never replace an existing file unless `overwrite=true` (this includes `project_save`, which rewrites the
project's own file).

| Tool | Remote API | What it does |
| --- | --- | --- |
| `project_info` | `GET /project` | file path, dirty flag, object / plate counts |
| `project_new` | `POST /project/new` | new empty project |
| `project_open` | `POST /project/open` | open a .3mf |
| `project_save` / `project_save_as` | `POST /project/save` | save to the project's file / to a new .3mf path |
| `project_import` | `POST /project/import` | add stl / obj / 3mf / step files to the plate |
| `project_export` | `POST /project/export` | 3mf copy, or stl / obj of the chosen objects or plate |
| `project_undo` / `project_redo` | `POST /project/undo`, `/project/redo` | Ctrl+Z / Ctrl+Y, several steps |
| `select_objects` | `PUT /selection` | select objects (or one part); empty list deselects |
| `object_rename` | `PUT /objects/{id}` | rename |
| `object_volumes` | `GET /objects/{id}/volumes` | parts, modifiers, negative volumes, support volumes (+ per-volume config) |
| `volume_add` | `POST /objects/{id}/volumes` | part / modifier / negative / support volume from a primitive or a file |
| `volume_set` | `PUT /objects/{id}/volumes/{i}` | rename, change type, per-volume settings |
| `volume_remove` | `DELETE /objects/{id}/volumes/{i}` | delete a volume (not the last model part) |
| `object_split` | `POST /objects/{id}/split` | split to objects, or a part to parts |
| `object_copy` | `POST /objects/{id}/copy` | independent copies (copy + paste); same-geometry instance is the stock `duplicate_object` |
| `object_mirror` | `POST /objects/{id}/mirror` | mirror along x / y / z |
| `object_scale_to_fit` | `POST /objects/{id}/scale_to_fit` | uniform scale into the plate's build volume |
| `object_drop_to_bed` | `POST /objects/{id}/drop` | drop to the bed |
| `object_cut` | `POST /objects/{id}/cut` | horizontal cut at Z, keep both / upper / lower |

Stock tools still cover delete (`delete_object`), instances (`duplicate_object`), transform, arrange and per-object
config (`set_object_config`).

### G3 - painting

Facets are chosen by a rule (all given criteria must hold): `fill`, height band `z_min`/`z_max`, direction
`normal` + `normal_angle`, `overhang_angle` (degrees from vertical; the gizmo's highlight angle is 90 minus it),
region `box_min`/`box_max`; `coverage` = `centroid` (default) / `any` / `all`. World coordinates, Z = 0 at the bed.
Painting works on the mesh's original triangles (no re-triangulation at band edges).

| Tool | Remote API | What it does |
| --- | --- | --- |
| `paint_color` | `POST /objects/{id}/paint` (kind=color) | colour (MMU) painting with a filament slot (mixed slots too) |
| `paint_seam` | `POST /objects/{id}/paint` (kind=seam) | seam enforce / block |
| `paint_fuzzy_skin` | `POST /objects/{id}/paint` (kind=fuzzy_skin) | fuzzy-skin painting |
| `paint_clear` | `POST /objects/{id}/paint/clear` | clear a kind: everything, or the facets a rule selects |
| `paint_summary` | `GET /objects/{id}/paint?kind=` | painted state: facet counts per part and state |

### G4 - layers and plates

`height` is the layer's print Z (mm). Custom G-code text needs Preferences > Remote API > "Allow script, G-code and
connection edits" (same policy as `*_gcode` keys in `PUT /config`).

| Tool | Remote API | What it does |
| --- | --- | --- |
| `layer_gcode_list` | `GET /layer_gcodes?plate=` | per-layer G-code items of a plate |
| `layer_gcode_add` | `POST /layer_gcodes` | colour change (M600), pause, tool change, custom G-code at a height |
| `layer_gcode_remove` | `DELETE /layer_gcodes` | remove by height, or all |
| `object_layers` | `GET /objects/{id}/layers` | height ranges + variable layer height profile summary |
| `layer_height_profile` | `PUT /objects/{id}/layer_height` (adaptive, reset) / `.../layer_height/smooth` | adaptive with quality factor, smooth, reset |
| `height_range_remove` | `DELETE /objects/{id}/height_range` | remove one range (stock `set_height_range` adds / edits) |
| `plate_settings` | `GET /plates/{i}/settings` | name, lock, bed type, print sequence, spiral vase, filament map mode |
| `plate_set` | `PUT /plates/{i}/settings` | change them (bed types only on printers with multiple bed types) |
| `plate_move` | `POST /plates/{i}/move` | reorder plates |

All mutating calls run on the UI thread and take an undo snapshot, so Ctrl+Z in OrcaSlicer reverts them.
