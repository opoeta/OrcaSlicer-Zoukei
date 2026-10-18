"""MCP tools for the Orca Zoukei colour features (group G0): filaments, mixed filaments, the colour
mixing engine and coloured-model import. They map 1:1 onto the Remote API endpoints documented at the
top of src/slic3r/GUI/RemoteAPI/RemoteAPIColor.cpp.

Filament numbers everywhere are the 1-based project slots shown in OrcaSlicer's sidebar (physical
filaments first, mixed filaments after them).
"""
from __future__ import annotations

from typing import Annotated, Literal

from pydantic import Field

from orcaslicer_mcp.errors import ApiError

from .client import zoukei_client


def _err(e: ApiError) -> dict:
    out = {"error": str(e)}
    errors = getattr(e, "errors", None)
    if errors:
        out["errors"] = errors
    return out


async def _call(method: str, path: str, body: dict | None = None, *, timeout: float = 120.0,
                params: dict | None = None) -> dict:
    try:
        async with zoukei_client() as c:
            return await c.call(method, path, body, timeout=timeout, params=params)
    except ApiError as e:
        return _err(e)


def register(mcp) -> None:
    @mcp.tool()
    async def filament_list() -> dict:
        """List every filament slot of the open project: 1-based slot number, whether it is a mixed
        filament, colour, multi-colour string, preset name, type and the transmission distance (TD, mm)
        the colour-mixing engine uses. Mixed slots also carry their recipe (components, percentages)
        and predicted colour."""
        return await _call("GET", "filaments")

    @mcp.tool()
    async def filament_set(
        slot: Annotated[int, Field(description="1-based project filament slot.")],
        preset: Annotated[str | None, Field(description="Filament preset name for this slot (physical slots only). Mixes only accept components of the same filament type.")] = None,
        colour: Annotated[str | None, Field(description="'#RRGGBB'. Physical filaments only (a mixed filament's colour follows its components).")] = None,
        multi_colour: Annotated[str | None, Field(description="Space-separated '#RRGGBB #RRGGBB ...' for a multi-colour (gradient/dual) filament.")] = None,
        td: Annotated[float | None, Field(description="HueForge-style transmission distance in mm (0 = unknown, 0..50). Used by the colour mixing engine to predict translucent filaments; stored in OrcaSlicer's settings, not in the project.")] = None,
    ) -> dict:
        """Change a filament slot's preset, colour, multi-colour fields and/or the TD the mixing engine uses."""
        body = {k: v for k, v in {"preset": preset, "colour": colour, "multi_colour": multi_colour, "td": td}.items() if v is not None}
        return await _call("PUT", f"filaments/{slot}", body)

    @mcp.tool()
    async def mixed_filament_list() -> dict:
        """List the project's mixed filaments (virtual slots that alternate their physical components
        layer by layer, or sub-layer by sub-layer): slot, components, percentages, predicted colour,
        gradient flag; plus the global mode ('layers' or 'sublayers') and the free slots left."""
        return await _call("GET", "mixed_filaments")

    @mcp.tool()
    async def mixed_filament_add(
        components: Annotated[list[int], Field(description="Physical filament numbers to mix (1-based), 2 or more, e.g. [1, 3].")],
        ratios: Annotated[list[int] | None, Field(description="Percentages, one per component (any positive integers; normalised). Omit when giving a pattern.")] = None,
        pattern: Annotated[str | None, Field(description="Layer pattern with letters A, B, C... naming the components in order, e.g. 'AAB' = 2 layers of the first for 1 of the second. Alternative to ratios.")] = None,
        mode: Annotated[Literal["layers", "sublayers", "auto"] | None, Field(description="How the mix is printed: whole 'layers' in a cadence, or 'sublayers' (each layer split per component, finer mix, more tool changes). Global print setting; 'auto' picks from the printer (tool changers -> sublayers). Omit to leave as is.")] = None,
        gradient: Annotated[bool, Field(description="Two components only: ramp the ratio from one to the other over the object's height.")] = False,
        gradient_direction: Annotated[int, Field(description="0 = start with the first component, 1 = start with the second (gradient only).")] = 0,
    ) -> dict:
        """Add a mixed filament (same code as 'Add Mixed Filament' in the sidebar). Reuses an identical
        existing fixed mix instead of creating a duplicate. Undoable with Ctrl+Z in OrcaSlicer."""
        body: dict = {"components": components, "gradient": gradient, "gradient_direction": gradient_direction}
        if ratios is not None:
            body["ratios"] = ratios
        if pattern is not None:
            body["pattern"] = pattern
        if mode is not None:
            body["mode"] = mode
        return await _call("POST", "mixed_filaments", body)

    @mcp.tool()
    async def mixed_filament_update(
        slot: Annotated[int, Field(description="1-based slot of an existing mixed filament (see mixed_filament_list).")],
        components: Annotated[list[int] | None, Field(description="New physical components (1-based). Omit to keep.")] = None,
        ratios: Annotated[list[int] | None, Field(description="New percentages. Omit to keep.")] = None,
        pattern: Annotated[str | None, Field(description="New layer pattern ('AAB'). Alternative to ratios.")] = None,
        mode: Annotated[Literal["layers", "sublayers", "auto"] | None, Field(description="Global mixing mode, see mixed_filament_add.")] = None,
        gradient: Annotated[bool | None, Field(description="Turn the two-component gradient on/off.")] = None,
        gradient_direction: Annotated[int, Field(description="0 or 1, gradient only.")] = 0,
    ) -> dict:
        """Edit a mixed filament in place (its slot number, and everything painted with it, stays)."""
        body: dict = {"gradient_direction": gradient_direction}
        for k, v in (("components", components), ("ratios", ratios), ("pattern", pattern), ("mode", mode), ("gradient", gradient)):
            if v is not None:
                body[k] = v
        return await _call("PUT", f"mixed_filaments/{slot}", body)

    @mcp.tool()
    async def mixed_filament_remove(
        slot: Annotated[int, Field(description="1-based slot of the mixed filament to delete.")],
    ) -> dict:
        """Delete a mixed filament. Anything painted with it falls back to a plain filament (undoable)."""
        return await _call("DELETE", f"mixed_filaments/{slot}")

    @mcp.tool()
    async def color_mix_solve(
        targets: Annotated[str | list, Field(description=(
            "What to reproduce: 'pending' = the colours of the model loaded with import_colored_model; "
            "'model' = the project filaments the model uses that are NOT loaded in the printer; or an explicit list "
            "of '#RRGGBB' strings / {hex, weight} objects."))] = "pending",
        loaded: Annotated[list[int] | None, Field(description="1-based physical filaments that are physically loaded and available to mix. Default: the first N (N = tool heads, else 4).")] = None,
        max_colors: Annotated[int | None, Field(description="k-means: cluster the targets into at most this many colours first (pending/colour lists).")] = None,
        max_mixes: Annotated[int | None, Field(description="At most this many new mixed filaments (each costs a filament slot).")] = None,
        allow_triples: Annotated[bool | None, Field(description="Also try 3-component mixes.")] = None,
        step: Annotated[int | None, Field(description="Ratio granularity: weights are multiples of 1/step (2,3,4,6,8,10).")] = None,
        method: Annotated[Literal["auto", "layers", "sublayers"] | None, Field(description="How the mixes will be printed.")] = None,
        prediction_model: Annotated[Literal["pigment", "optical"] | None, Field(description="Colour prediction model: 'pigment' (default, calibrated against Bambu's measured blends) or 'optical'.")] = None,
        only_physical: Annotated[bool | None, Field(description="No mixes: only match the nearest loaded filament.")] = None,
        td: Annotated[dict[str, float] | None, Field(description="Transmission distance overrides in mm keyed by filament number, e.g. {'3': 4.5}.")] = None,
        used_only: Annotated[bool, Field(description="targets='model' only: restrict to filaments the model actually uses.")] = True,
    ) -> dict:
        """Compute recipes (physical filament or mix with integer ratios / layer pattern) for target
        colours, with the predicted colour and the CIEDE2000 error of each, WITHOUT changing the project.
        Returns a solve_id for color_mix_apply, per-cluster choices, 'assignments' (target -> cluster)
        and mean/max delta E against the physical-only baseline."""
        body: dict = {"targets": targets, "used_only": used_only}
        for k, v in (("loaded", loaded), ("max_colors", max_colors), ("max_mixes", max_mixes), ("allow_triples", allow_triples),
                     ("step", step), ("method", method), ("prediction_model", prediction_model), ("only_physical", only_physical), ("td", td)):
            if v is not None:
                body[k] = v
        return await _call("POST", "color_mix/solve", body)

    @mcp.tool()
    async def color_mix_apply(
        solve_id: Annotated[int, Field(description="solve_id returned by color_mix_solve (kept for the last 8 solves).")],
        overrides: Annotated[dict[str, dict] | None, Field(description=(
            "Change what a cluster gets, keyed by cluster number: {'2': {'filament': 3}} or "
            "{'2': {'components': [1, 2], 'percentages': [75, 25]}}."))] = None,
    ) -> dict:
        """Apply a solve result: create the mixed filaments (reusing existing ones), set the printing mode,
        then repaint the model (pending import) or remap the filaments the model used (targets='model').
        One undo step in OrcaSlicer. Returns the created mixes and every assignment."""
        body: dict = {"solve_id": solve_id}
        if overrides:
            body["overrides"] = overrides
        return await _call("POST", "color_mix/apply", body)

    @mcp.tool()
    async def model_colors() -> dict:
        """Colours of the model: the pending coloured import (clusters with colour, surface share, faces)
        and the filaments the objects' parts currently print with."""
        return await _call("GET", "model/colors")

    @mcp.tool()
    async def import_colored_model(
        path: Annotated[str, Field(description="OBJ (MTL / vertex colours), textured OBJ or GLB/3MF path on the OrcaSlicer host.")],
        max_colors: Annotated[int, Field(description="Cluster the model's colours into at most this many (0 = automatic).")] = 0,
        smooth: Annotated[int, Field(description="Colour-boundary smoothing 0..10 (the import dialog's slider).")] = 5,
    ) -> dict:
        """Load a coloured model WITHOUT opening the texture/colour dialog: the geometry goes on the plate
        uncoloured, its colours are clustered and returned as a palette. Next: color_mix_solve with
        targets='pending', then color_mix_apply (paints the model and creates the mixes)."""
        return await _call("POST", "model/import_colored", {"path": path, "max_colors": max_colors, "smooth": smooth}, timeout=240.0)

    @mcp.tool()
    async def set_object_filament(
        object_id: Annotated[int, Field(description="Object id from list_objects.")],
        filament: Annotated[int, Field(description="1-based filament slot (mixed slots allowed); 0 = back to the default.")],
        volume: Annotated[int | None, Field(description="Volume (part) index to set instead of the whole object.")] = None,
    ) -> dict:
        """Assign a filament to an object, or to one of its parts (volumes)."""
        body: dict = {"filament": filament}
        if volume is not None:
            body["volume"] = volume
        return await _call("PUT", f"objects/{object_id}/filament", body)
