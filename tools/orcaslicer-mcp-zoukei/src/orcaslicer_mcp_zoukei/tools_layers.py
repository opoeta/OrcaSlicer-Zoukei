"""MCP tools for the Orca Zoukei layers and plates group (G4): per-layer G-code (pause, colour change,
tool change, custom text), layer height ranges / variable layer height, and per-plate settings. They map
onto the endpoints documented at the top of src/slic3r/GUI/RemoteAPI/RemoteAPILayers.cpp.

'height' is the layer's print Z (top of the layer, mm): the item applies at the first layer whose print Z
reaches it. Stock tools set_layer_height / set_height_range / list_plates keep working."""
from __future__ import annotations

from typing import Annotated, Literal

from pydantic import Field

from .tools_common import ObjectId, call, clean

GcodeType = Literal["color_change", "pause", "tool_change", "custom"]
Plate = Annotated[int, Field(description="0-based plate index (list_plates).")]


def register(mcp) -> None:
    @mcp.tool()
    async def layer_gcode_list(
        plate: Annotated[int | None, Field(description="0-based plate index. Default: the current plate.")] = None,
    ) -> dict:
        """List the per-layer G-code items of a plate (colour changes, pauses, tool changes, custom G-code)."""
        return await call("GET", "layer_gcodes", params=clean(plate=plate) or None)

    @mcp.tool()
    async def layer_gcode_add(
        type: Annotated[GcodeType, Field(description="color_change (M600 / filament swap) | pause | tool_change (switch to extruder) | custom (your G-code text).")],
        height: Annotated[float, Field(description="Layer print Z in mm where it applies (e.g. 4.2).")],
        plate: Annotated[int | None, Field(description="0-based plate index. Default: the current plate.")] = None,
        extruder: Annotated[int, Field(description="color_change / tool_change: 1-based filament slot.")] = 1,
        color: Annotated[str | None, Field(description="color_change: '#RRGGBB' of the new filament colour. Default: the slot's colour.")] = None,
        message: Annotated[str | None, Field(description="pause: message shown on the printer.")] = None,
        gcode: Annotated[str | None, Field(description="custom: G-code text, lines separated by \\n. Needs 'Allow script, G-code and connection edits' in Preferences > Remote API.")] = None,
        replace: Annotated[bool, Field(description="Replace an item that already sits on that layer.")] = False,
    ) -> dict:
        """Add a per-layer G-code item at a height (undoable). The plate's slice becomes stale: re-slice
        afterwards."""
        body = clean(type=type, height=height, plate=plate, message=message, gcode=gcode, color=color)
        if type in ("color_change", "tool_change"):
            body["extruder"] = extruder
        if replace:
            body["replace"] = True
        return await call("POST", "layer_gcodes", body)

    @mcp.tool()
    async def layer_gcode_remove(
        height: Annotated[float | None, Field(description="Layer print Z of the item to remove.")] = None,
        plate: Annotated[int | None, Field(description="0-based plate index. Default: the current plate.")] = None,
        type: Annotated[GcodeType | None, Field(description="Only items of this type.")] = None,
        all: Annotated[bool, Field(description="Remove every item of the plate (optionally only one type) instead of one height.")] = False,
    ) -> dict:
        """Remove the per-layer G-code item at a height, or all of them (undoable)."""
        body = clean(height=height, plate=plate, type=type)
        if all:
            body["all"] = True
        return await call("DELETE", "layer_gcodes", body)

    @mcp.tool()
    async def object_layers(object_id: ObjectId) -> dict:
        """Layer settings of an object: base layer height, height ranges (with their overrides) and a summary
        of the variable layer height profile (custom flag, points, min / max height)."""
        return await call("GET", f"objects/{object_id}/layers")

    @mcp.tool()
    async def layer_height_profile(
        object_id: ObjectId,
        mode: Annotated[Literal["adaptive", "smooth", "reset"], Field(description="adaptive = compute variable layer heights from the surface; smooth = smooth the current profile; reset = back to the fixed layer height.")],
        quality: Annotated[float, Field(description="adaptive: 0 (fast, thick) .. 1 (quality, thin layers).")] = 0.5,
        radius: Annotated[int, Field(description="smooth: smoothing radius 1..50.")] = 5,
        keep_min: Annotated[bool, Field(description="smooth: keep the minimum (thin) layers.")] = False,
    ) -> dict:
        """Variable layer height of an object: adaptive (with a quality factor), smooth, or reset. Re-slice
        afterwards."""
        if mode == "smooth":
            return await call("PUT", f"objects/{object_id}/layer_height/smooth", {"radius": radius, "keep_min": keep_min})
        body: dict = {"mode": mode}
        if mode == "adaptive":
            body["quality"] = quality
        return await call("PUT", f"objects/{object_id}/layer_height", body)

    @mcp.tool()
    async def height_range_remove(
        object_id: ObjectId,
        min_z: Annotated[float, Field(description="Lower Z of the range, exactly as listed by object_layers.")],
        max_z: Annotated[float, Field(description="Upper Z of the range, exactly as listed.")],
    ) -> dict:
        """Remove one layer-height range (the stock set_height_range adds / edits / clears all)."""
        return await call("DELETE", f"objects/{object_id}/height_range", {"min_z": min_z, "max_z": max_z})

    @mcp.tool()
    async def plate_settings(plate: Plate) -> dict:
        """Settings of one plate: name, lock, bed type (own and effective), print sequence (own and
        effective), spiral vase, filament map mode."""
        return await call("GET", f"plates/{plate}/settings")

    @mcp.tool()
    async def plate_set(
        plate: Plate,
        name: Annotated[str | None, Field(description="Plate name (max 100 characters).")] = None,
        locked: Annotated[bool | None, Field(description="Lock / unlock the plate.")] = None,
        bed_type: Annotated[str | None, Field(description="default | cool_plate | eng_plate | hot_plate | textured_plate | textured_cool_plate | supertack_plate. Only on printers with multiple bed types.")] = None,
        print_sequence: Annotated[Literal["by_layer", "by_object", "default"] | None, Field(description="Print sequence of this plate; default = follow the print preset.")] = None,
        spiral_vase: Annotated[Literal["global"] | bool | None, Field(description="'global' = follow the preset; False = off for this plate. (Turning it on needs the confirmation dialog in the window.)")] = None,
        filament_map_mode: Annotated[Literal["default", "auto_for_flush", "auto_for_match"] | None, Field(description="Multi-extruder printers only.")] = None,
    ) -> dict:
        """Change plate settings (undoable): name, lock, bed type, print sequence, spiral vase, filament map
        mode. Only the fields you give change."""
        body = clean(name=name, locked=locked, bed_type=bed_type, print_sequence=print_sequence,
                     spiral_vase=spiral_vase, filament_map_mode=filament_map_mode)
        return await call("PUT", f"plates/{plate}/settings", body)

    @mcp.tool()
    async def plate_move(
        plate: Plate,
        to: Annotated[int, Field(description="New 0-based position of the plate.")],
    ) -> dict:
        """Reorder plates: move a plate to another position (objects travel with it; undoable)."""
        return await call("POST", f"plates/{plate}/move", {"to": to})
