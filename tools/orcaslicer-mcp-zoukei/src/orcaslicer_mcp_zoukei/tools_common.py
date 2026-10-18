"""Helpers shared by the Zoukei MCP tool modules of groups G1-G4 (supports, project/model operations,
painting, layers and plates)."""
from __future__ import annotations

from typing import Annotated

from pydantic import Field

from orcaslicer_mcp.errors import ApiError

from .client import zoukei_client


def err(e: ApiError) -> dict:
    out = {"error": str(e)}
    errors = getattr(e, "errors", None)
    if errors:
        out["errors"] = errors
    return out


async def call(method: str, path: str, body: dict | None = None, *, timeout: float = 120.0,
               params: dict | None = None) -> dict:
    """One Remote API request; HTTP errors come back as {"error": ...} instead of raising."""
    try:
        async with zoukei_client() as c:
            return await c.call(method, path, body, timeout=timeout, params=params)
    except ApiError as e:
        return err(e)


def clean(**kw) -> dict:
    """The keyword arguments that are not None."""
    return {k: v for k, v in kw.items() if v is not None}


def build_rule(*, fill: bool = False, overhang_angle: float | None = None, z_min: float | None = None,
               z_max: float | None = None, normal: list[float] | None = None, normal_angle: float | None = None,
               box_min: list[float] | None = None, box_max: list[float] | None = None,
               coverage: str | None = None) -> dict:
    """The facet-selection rule of the painting endpoints (RemoteAPIUtil.cpp, parse_rule)."""
    rule = clean(overhang_angle=overhang_angle, z_min=z_min, z_max=z_max, normal=normal,
                 normal_angle=normal_angle, box_min=box_min, box_max=box_max, coverage=coverage)
    if fill:
        rule["fill"] = True
    return rule


# Parameter types reused by the painting tools. Facets are chosen by a rule: every criterion given must hold
# (AND); world coordinates, Z = 0 at the bed; painting works on the mesh's original triangles.
ObjectId = Annotated[int, Field(description="Object id from list_objects.")]
Volume = Annotated[int | None, Field(description="Volume (part) index from object_volumes. Omit = every model part.")]
Fill = Annotated[bool, Field(description="Select every facet (alone, or together with other criteria).")]
OverhangAngle = Annotated[float | None, Field(description="Degrees from vertical, 0..90: facets leaning down more than this.")]
ZMin = Annotated[float | None, Field(description="World Z (mm) lower bound.")]
ZMax = Annotated[float | None, Field(description="World Z (mm) upper bound.")]
Normal = Annotated[list[float] | None, Field(description="Direction [x, y, z] the facets must face, e.g. [0,0,1] = up.")]
NormalAngle = Annotated[float | None, Field(description="Tolerance in degrees around normal (default 30).")]
BoxMin = Annotated[list[float] | None, Field(description="Region corner [x, y, z] (world mm); needs box_max.")]
BoxMax = Annotated[list[float] | None, Field(description="Opposite region corner [x, y, z]; needs box_min.")]
Coverage = Annotated[str | None, Field(description="'centroid' (default) | 'any' | 'all'.")]
