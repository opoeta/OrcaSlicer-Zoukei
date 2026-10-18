"""MCP tools for the Orca Zoukei painting group (G3): colour (MMU), seam and fuzzy-skin painting by rules,
clear and summary. They map onto the endpoints documented at the top of
src/slic3r/GUI/RemoteAPI/RemoteAPIPaint.cpp.

Painting rules: every criterion given must hold (AND); world coordinates, Z = 0 at the bed. fill = every
facet; z_min/z_max = height band; normal + normal_angle = facets facing a direction (up = [0,0,1]);
overhang_angle = facets leaning down by more than this many degrees from vertical; box_min/box_max = region.
Triangles are tested by their centroid unless coverage is 'any' (touches the range) or 'all' (fully inside).
Painting works on the mesh's original triangles (no re-triangulation at band edges)."""
from __future__ import annotations

from typing import Annotated, Literal

from pydantic import Field

from .tools_common import (BoxMax, BoxMin, Coverage, Fill, Normal, NormalAngle, ObjectId, OverhangAngle, Volume,
                           ZMax, ZMin, build_rule, call, clean)


def register(mcp) -> None:
    @mcp.tool()
    async def paint_color(
        object_id: ObjectId,
        filament: Annotated[int, Field(description="1-based project filament slot to paint with (mixed slots allowed; see filament_list).")],
        fill: Fill = False,
        z_min: ZMin = None,
        z_max: ZMax = None,
        normal: Normal = None,
        normal_angle: NormalAngle = None,
        overhang_angle: OverhangAngle = None,
        box_min: BoxMin = None,
        box_max: BoxMax = None,
        coverage: Coverage = None,
        volume: Volume = None,
    ) -> dict:
        """Colour (multi-material) painting by facet rules: a height band (z_min/z_max), the facets facing a
        direction (normal, e.g. [0,0,1] = top surfaces), the overhangs, a region, or fill=True for the whole
        object / volume. Replaces any colour already painted on the selected facets. Undoable."""
        rule = build_rule(fill=fill, overhang_angle=overhang_angle, z_min=z_min, z_max=z_max, normal=normal,
                          normal_angle=normal_angle, box_min=box_min, box_max=box_max, coverage=coverage)
        body = clean(kind="color", state=filament, volume=volume)
        body["rule"] = rule
        return await call("POST", f"objects/{object_id}/paint", body)

    @mcp.tool()
    async def paint_seam(
        object_id: ObjectId,
        state: Annotated[Literal["enforce", "block"], Field(description="enforce = put the seam here; block = keep the seam out.")],
        fill: Fill = False,
        z_min: ZMin = None,
        z_max: ZMax = None,
        normal: Normal = None,
        normal_angle: NormalAngle = None,
        overhang_angle: OverhangAngle = None,
        box_min: BoxMin = None,
        box_max: BoxMax = None,
        coverage: Coverage = None,
        volume: Volume = None,
    ) -> dict:
        """Seam painting (enforce / block) on the facets a rule selects. Undoable."""
        rule = build_rule(fill=fill, overhang_angle=overhang_angle, z_min=z_min, z_max=z_max, normal=normal,
                          normal_angle=normal_angle, box_min=box_min, box_max=box_max, coverage=coverage)
        body = clean(kind="seam", state=state, volume=volume)
        body["rule"] = rule
        return await call("POST", f"objects/{object_id}/paint", body)

    @mcp.tool()
    async def paint_fuzzy_skin(
        object_id: ObjectId,
        fill: Fill = False,
        z_min: ZMin = None,
        z_max: ZMax = None,
        normal: Normal = None,
        normal_angle: NormalAngle = None,
        overhang_angle: OverhangAngle = None,
        box_min: BoxMin = None,
        box_max: BoxMax = None,
        coverage: Coverage = None,
        volume: Volume = None,
    ) -> dict:
        """Fuzzy-skin painting on the facets a rule selects (the fuzzy skin itself is configured in the
        process settings). Undoable."""
        rule = build_rule(fill=fill, overhang_angle=overhang_angle, z_min=z_min, z_max=z_max, normal=normal,
                          normal_angle=normal_angle, box_min=box_min, box_max=box_max, coverage=coverage)
        body = clean(kind="fuzzy_skin", state="on", volume=volume)
        body["rule"] = rule
        return await call("POST", f"objects/{object_id}/paint", body)

    @mcp.tool()
    async def paint_clear(
        object_id: ObjectId,
        kind: Annotated[Literal["color", "seam", "fuzzy_skin", "support"], Field(description="Which painting to clear.")],
        fill: Fill = False,
        z_min: ZMin = None,
        z_max: ZMax = None,
        normal: Normal = None,
        normal_angle: NormalAngle = None,
        overhang_angle: OverhangAngle = None,
        box_min: BoxMin = None,
        box_max: BoxMax = None,
        coverage: Coverage = None,
        volume: Volume = None,
    ) -> dict:
        """Clear one kind of painting: everything (no criterion given), or only the facets a rule selects.
        Undoable."""
        body = clean(kind=kind, volume=volume)
        rule = build_rule(fill=fill, overhang_angle=overhang_angle, z_min=z_min, z_max=z_max, normal=normal,
                          normal_angle=normal_angle, box_min=box_min, box_max=box_max, coverage=coverage)
        if rule:
            body["rule"] = rule
        return await call("POST", f"objects/{object_id}/paint/clear", body)

    @mcp.tool()
    async def paint_summary(
        object_id: ObjectId,
        kind: Annotated[Literal["color", "seam", "fuzzy_skin", "support"] | None, Field(description="Only this kind. Omit = all four.")] = None,
    ) -> dict:
        """Painted state of an object: per model part and kind, whether it is painted and the facet count per
        state (filament slot for colour, enforcer / blocker for seam and support)."""
        return await call("GET", f"objects/{object_id}/paint", params=clean(kind=kind) or None)
