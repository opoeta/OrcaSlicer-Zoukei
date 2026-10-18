"""MCP tools for the Orca Zoukei supports group (G1): support enforcer / blocker volumes and painting.
They map 1:1 onto the endpoints documented at the top of src/slic3r/GUI/RemoteAPI/RemoteAPISupport.cpp.
Per-object support settings (type, threshold angle, ...) are set with the stock set_object_config tool.

Painting rules (support_paint, support_paint_clear): every criterion given must hold (AND); world
coordinates, Z = 0 at the bed. fill = every facet; overhang_angle = facets leaning down by more than this
many degrees from vertical (45 = classic support threshold; the gizmo's 'highlight overhangs' angle is 90
minus this); z_min/z_max = height band; normal + normal_angle = facets facing a direction; box_min/box_max =
region. Triangles are tested by their centroid unless coverage is 'any' or 'all'."""
from __future__ import annotations

from typing import Annotated, Literal

from pydantic import Field

from .tools_common import (BoxMax, BoxMin, Coverage, Fill, Normal, NormalAngle, ObjectId, OverhangAngle, Volume,
                           ZMax, ZMin, build_rule, call, clean)


def register(mcp) -> None:
    @mcp.tool()
    async def support_volume_list(object_id: ObjectId) -> dict:
        """List the support enforcer / blocker volumes of an object (index, name, size, centre in world mm)."""
        return await call("GET", f"objects/{object_id}/support_volumes")

    @mcp.tool()
    async def support_volume_add(
        object_id: ObjectId,
        kind: Annotated[Literal["enforcer", "blocker"], Field(description="enforcer = force supports inside the volume; blocker = forbid them.")],
        shape: Annotated[Literal["box", "cylinder", "sphere"] | None, Field(description="Primitive. Give shape+size, or file.")] = None,
        size: Annotated[list[float] | None, Field(description="[x, y, z] in mm, world-aligned. Cylinder: [diameter_x, diameter_y, height]; sphere: diameters.")] = None,
        position: Annotated[list[float] | None, Field(description="World [x, y, z] of the volume's centre (mm). Default: the centre of the object.")] = None,
        file: Annotated[str | None, Field(description="STL / OBJ / 3MF path on the OrcaSlicer host, instead of a primitive.")] = None,
        scale: Annotated[float, Field(description="Scale factor for a mesh file.")] = 1.0,
        name: Annotated[str | None, Field(description="Volume name.")] = None,
    ) -> dict:
        """Add a support enforcer or blocker modifier volume to an object (undoable). Supports are only
        forced / blocked where the volume overlaps the object."""
        body = clean(kind=kind, shape=shape, size=size, position=position, file=file, name=name)
        if file is not None:
            body["scale"] = scale
        return await call("POST", f"objects/{object_id}/support_volumes", body)

    @mcp.tool()
    async def support_volume_remove(
        object_id: ObjectId,
        volume: Annotated[int | None, Field(description="Volume index (from support_volume_list) to remove.")] = None,
        all: Annotated[bool, Field(description="Remove every support volume (optionally only one kind).")] = False,
        kind: Annotated[Literal["enforcer", "blocker"] | None, Field(description="With all=True: only this kind.")] = None,
    ) -> dict:
        """Remove one support enforcer/blocker volume, or all of them (undoable)."""
        body = clean(volume=volume, kind=kind)
        if all:
            body["all"] = True
        return await call("DELETE", f"objects/{object_id}/support_volumes", body)

    @mcp.tool()
    async def support_paint(
        object_id: ObjectId,
        state: Annotated[Literal["enforcer", "blocker"], Field(description="Paint support enforcers or blockers.")],
        fill: Fill = False,
        overhang_angle: OverhangAngle = None,
        z_min: ZMin = None,
        z_max: ZMax = None,
        normal: Normal = None,
        normal_angle: NormalAngle = None,
        box_min: BoxMin = None,
        box_max: BoxMax = None,
        coverage: Coverage = None,
        volume: Volume = None,
    ) -> dict:
        """Paint support enforcers/blockers on the facets a rule selects (the support paint gizmo's
        'highlight overhangs' + fill, by numbers). Example: overhang_angle=45, state='enforcer' paints every
        facet leaning down more than 45 degrees from vertical. Give at least one criterion (fill=True for all).
        Undoable."""
        rule = build_rule(fill=fill, overhang_angle=overhang_angle, z_min=z_min, z_max=z_max, normal=normal,
                          normal_angle=normal_angle, box_min=box_min, box_max=box_max, coverage=coverage)
        body = clean(state=state, volume=volume)
        body["rule"] = rule
        return await call("POST", f"objects/{object_id}/support_paint", body)

    @mcp.tool()
    async def support_paint_clear(
        object_id: ObjectId,
        fill: Fill = False,
        overhang_angle: OverhangAngle = None,
        z_min: ZMin = None,
        z_max: ZMax = None,
        normal: Normal = None,
        normal_angle: NormalAngle = None,
        box_min: BoxMin = None,
        box_max: BoxMax = None,
        coverage: Coverage = None,
        volume: Volume = None,
    ) -> dict:
        """Clear painted support enforcers/blockers: everything (no criterion given) or only the facets a
        rule selects. Undoable."""
        body = clean(volume=volume)
        rule = build_rule(fill=fill, overhang_angle=overhang_angle, z_min=z_min, z_max=z_max, normal=normal,
                          normal_angle=normal_angle, box_min=box_min, box_max=box_max, coverage=coverage)
        if rule:
            body["rule"] = rule
        return await call("POST", f"objects/{object_id}/support_paint/clear", body)

    @mcp.tool()
    async def support_paint_summary(object_id: ObjectId) -> dict:
        """Painted support state of an object: per model part, whether it is painted and how many facets are
        enforcers / blockers."""
        return await call("GET", f"objects/{object_id}/support_paint")
