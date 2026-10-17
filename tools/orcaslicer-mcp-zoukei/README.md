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
