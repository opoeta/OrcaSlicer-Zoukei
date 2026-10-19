"""MCP tools for the Orca Zoukei project / model operations group (G2): new / open / save / import / export,
undo / redo, selection, and object and volume (part, modifier, negative volume) editing. They map onto the
endpoints documented at the top of src/slic3r/GUI/RemoteAPI/RemoteAPIProject.cpp.

Safety: project_new / project_open refuse when the project has unsaved changes unless discard_changes=True;
every file write needs the exact path and refuses to replace an existing file unless overwrite=True."""
from __future__ import annotations

from typing import Annotated, Literal

from pydantic import Field

from orcaslicer_mcp.guard import check_placeholders, sensitive_keys

from .tools_common import ObjectId, call, clean

VolumeIndex = Annotated[int, Field(description="Volume index from object_volumes.")]
VolumeType = Literal["part", "modifier", "negative", "support_enforcer", "support_blocker"]


def register(mcp) -> None:
    # ---- project -----------------------------------------------------------------------------
    @mcp.tool()
    async def project_info() -> dict:
        """The open project: file path (empty if never saved), whether it has unsaved changes, object and
        plate counts."""
        return await call("GET", "project")

    @mcp.tool()
    async def project_new(
        discard_changes: Annotated[bool, Field(description="Allow throwing away unsaved changes. Without it a dirty project is refused (409).")] = False,
    ) -> dict:
        """Start a new empty project. DESTRUCTIVE when the current project is unsaved: it is refused unless
        discard_changes=True."""
        return await call("POST", "project/new", {"discard_changes": discard_changes}, timeout=180.0)

    @mcp.tool()
    async def project_open(
        path: Annotated[str, Field(description="A .3mf project on the OrcaSlicer host.")],
        discard_changes: Annotated[bool, Field(description="Allow throwing away unsaved changes (project and modified presets).")] = False,
    ) -> dict:
        """Open a .3mf project. DESTRUCTIVE when the current project is unsaved: refused unless
        discard_changes=True."""
        return await call("POST", "project/open", {"path": path, "discard_changes": discard_changes}, timeout=360.0)

    @mcp.tool()
    async def project_save(
        overwrite: Annotated[bool, Field(description="Must be True: saving writes over the project's own existing file.")] = False,
    ) -> dict:
        """Save the project to the file it already has. Refused (exists) unless overwrite=True, because it
        replaces that file. A never-saved project needs project_save_as."""
        return await call("POST", "project/save", {"overwrite": overwrite}, timeout=360.0)

    @mcp.tool()
    async def project_save_as(
        path: Annotated[str, Field(description="Target .3mf path (its folder must exist).")],
        overwrite: Annotated[bool, Field(description="Allow replacing an existing file.")] = False,
    ) -> dict:
        """Save the project to a new .3mf path; it becomes the project's file. Never replaces an existing
        file unless overwrite=True."""
        return await call("POST", "project/save", {"path": path, "overwrite": overwrite}, timeout=360.0)

    @mcp.tool()
    async def project_import(
        paths: Annotated[list[str], Field(description="Model files on the OrcaSlicer host: .stl .obj .3mf .step .stp. Each becomes one or more objects on the current plate.")],
    ) -> dict:
        """Import one or more model files into the open project (undoable). Per-file result with the new
        objects' ids and sizes."""
        return await call("POST", "project/import", {"paths": paths}, timeout=360.0)

    @mcp.tool()
    async def project_export(
        format: Annotated[Literal["3mf", "stl", "obj"], Field(description="3mf = a copy of the whole project (the project file is unchanged); stl / obj = the printable geometry of the chosen objects in world coordinates as one mesh.")],
        path: Annotated[str, Field(description="Target path with the matching extension (its folder must exist).")],
        overwrite: Annotated[bool, Field(description="Allow replacing an existing file.")] = False,
        objects: Annotated[list[int] | None, Field(description="stl / obj: only these object ids. Default: all.")] = None,
        plate: Annotated[int | None, Field(description="stl / obj: only the instances on this plate (0-based).")] = None,
    ) -> dict:
        """Export the project (3mf copy) or the printable geometry (stl / obj). Never replaces an existing
        file unless overwrite=True."""
        body = clean(format=format, path=path, overwrite=overwrite, objects=objects, plate=plate)
        return await call("POST", "project/export", body, timeout=360.0)

    @mcp.tool()
    async def export_glb(
        path: Annotated[str, Field(description="Target .glb path (its folder must exist).")],
        embed_html: Annotated[bool, Field(description="Also write a self-contained web viewer: a single .html (same name) with the GLB embedded, to share by e-mail or chat. False writes no HTML.")] = False,
        travel: Annotated[bool, Field(description="Include travel moves (thin lines, hidden by default in the viewer).")] = False,
        detail: Annotated[Literal["preview", "standard", "full"], Field(description="preview = smallest file, standard = default, full = every segment with rounded (octagon) beads.")] = "standard",
        overwrite: Annotated[bool, Field(description="Allow replacing existing files.")] = False,
    ) -> dict:
        """Export the sliced G-code preview toolpaths of the current plate as a GLB (glTF 2.0 binary): beads as
        tube meshes in the real filament colours, one node per feature role, print stats in extras. Needs a
        sliced plate shown in the Preview. Writes only to path (and the .html next to it); never replaces
        an existing file unless overwrite=True."""
        body = clean(path=path, embed_html=embed_html, travel=travel, detail=detail, overwrite=overwrite)
        return await call("POST", "export/glb", body, timeout=600.0)

    @mcp.tool()
    async def project_undo(
        steps: Annotated[int, Field(description="How many undo steps (1..50).")] = 1,
    ) -> dict:
        """Undo the last step(s), like Ctrl+Z (needs the 3D editor tab to be shown)."""
        return await call("POST", "project/undo", {"steps": steps})

    @mcp.tool()
    async def project_redo(
        steps: Annotated[int, Field(description="How many redo steps (1..50).")] = 1,
    ) -> dict:
        """Redo undone step(s), like Ctrl+Y."""
        return await call("POST", "project/redo", {"steps": steps})

    @mcp.tool()
    async def select_objects(
        objects: Annotated[list[int], Field(description="Object ids to select; an empty list deselects everything.")],
        volume: Annotated[int | None, Field(description="With one object: select this part instead of the whole object.")] = None,
    ) -> dict:
        """Select objects (or one part) in the window, so the 3D view and the object list show them."""
        return await call("PUT", "selection", clean(objects=objects, volume=volume))

    # ---- objects ------------------------------------------------------------------------------
    @mcp.tool()
    async def object_rename(object_id: ObjectId, name: Annotated[str, Field(description="New name, 1..200 characters.")]) -> dict:
        """Rename an object (undoable)."""
        return await call("PUT", f"objects/{object_id}", {"name": name})

    @mcp.tool()
    async def object_volumes(object_id: ObjectId) -> dict:
        """List the volumes of an object: model parts, modifiers, negative volumes and support
        enforcers / blockers, with index, type, size, centre (world), facet count, painted flags and per-volume
        config."""
        return await call("GET", f"objects/{object_id}/volumes")

    @mcp.tool()
    async def volume_add(
        object_id: ObjectId,
        type: Annotated[VolumeType, Field(description="part = an extra model part; modifier = settings volume (use volume_set config); negative = subtracts material; support_enforcer / support_blocker.")],
        shape: Annotated[Literal["box", "cylinder", "sphere"] | None, Field(description="Primitive. Give shape+size, or file.")] = None,
        size: Annotated[list[float] | None, Field(description="[x, y, z] mm, world-aligned. Cylinder: [diameter_x, diameter_y, height]; sphere: diameters.")] = None,
        position: Annotated[list[float] | None, Field(description="World [x, y, z] of the volume's centre (mm). Default: the centre of the object.")] = None,
        file: Annotated[str | None, Field(description="STL / OBJ / 3MF path on the OrcaSlicer host, instead of a primitive.")] = None,
        scale: Annotated[float, Field(description="Scale factor for a mesh file.")] = 1.0,
        name: Annotated[str | None, Field(description="Volume name.")] = None,
    ) -> dict:
        """Add a part, modifier, negative volume or support volume to an object (undoable). Volume indices
        may change afterwards (OrcaSlicer orders parts first): use the returned 'volume' index."""
        body = clean(type=type, shape=shape, size=size, position=position, file=file, name=name)
        if file is not None:
            body["scale"] = scale
        return await call("POST", f"objects/{object_id}/volumes", body)

    @mcp.tool()
    async def volume_set(
        object_id: ObjectId,
        volume: VolumeIndex,
        name: Annotated[str | None, Field(description="New name.")] = None,
        type: Annotated[VolumeType | None, Field(description="Change the volume's type (an object keeps at least one model part).")] = None,
        config: Annotated[dict | None, Field(description="Per-volume settings {key: value}; null removes the override. Same keys as set_object_config. Script / G-code / connection keys are refused.")] = None,
    ) -> dict:
        """Rename a volume, change its type and/or set its per-volume settings (undoable)."""
        if config:
            check_placeholders(config)
            hits = sensitive_keys(config)
            if hits:
                return {"error": "blocked: script, G-code and connection keys cannot be set through the API", "keys": hits}
        return await call("PUT", f"objects/{object_id}/volumes/{volume}", clean(name=name, type=type, config=config))

    @mcp.tool()
    async def volume_remove(object_id: ObjectId, volume: VolumeIndex) -> dict:
        """Delete a volume (part, modifier, negative volume, support volume). The last model part cannot be
        deleted. Undoable."""
        return await call("DELETE", f"objects/{object_id}/volumes/{volume}")

    @mcp.tool()
    async def object_split(
        object_id: ObjectId,
        mode: Annotated[Literal["objects", "parts"], Field(description="objects = every connected piece becomes its own object; parts = pieces stay in this object as separate parts.")] = "objects",
        volume: Annotated[int | None, Field(description="mode='parts' on a multi-part object: which part to split.")] = None,
    ) -> dict:
        """Split an object into separate objects, or one of its parts into parts (undoable)."""
        return await call("POST", f"objects/{object_id}/split", clean(mode=mode, volume=volume))

    @mcp.tool()
    async def object_copy(
        object_id: ObjectId,
        copies: Annotated[int, Field(description="1..20 independent copies.")] = 1,
    ) -> dict:
        """Copy an object as new independent objects (copy + paste). For another instance of the same object
        (shared geometry) use the stock duplicate_object. Needs the 3D editor tab to be shown."""
        return await call("POST", f"objects/{object_id}/copy", {"copies": copies})

    @mcp.tool()
    async def object_mirror(object_id: ObjectId, axis: Annotated[Literal["x", "y", "z"], Field(description="Axis to mirror along (object axes).")]) -> dict:
        """Mirror an object along an axis (undoable)."""
        return await call("POST", f"objects/{object_id}/mirror", {"axis": axis})

    @mcp.tool()
    async def object_scale_to_fit(
        object_id: ObjectId,
        margin: Annotated[float, Field(description="Keep this many mm free around the object (0..100).")] = 0.0,
        allow_upscale: Annotated[bool, Field(description="Also enlarge a small object to fill the plate (default: only shrink).")] = False,
        center: Annotated[bool, Field(description="Centre the object on the plate afterwards.")] = False,
    ) -> dict:
        """Scale an object uniformly so it fits the plate's build volume (undoable). No-op if it already fits
        and allow_upscale is False."""
        return await call("POST", f"objects/{object_id}/scale_to_fit", {"margin": margin, "allow_upscale": allow_upscale, "center": center})

    @mcp.tool()
    async def object_drop_to_bed(object_id: ObjectId) -> dict:
        """Move an object straight down (or up) so its lowest point touches the bed (undoable)."""
        return await call("POST", f"objects/{object_id}/drop")

    @mcp.tool()
    async def object_cut(
        object_id: ObjectId,
        z: Annotated[float, Field(description="World Z (mm) of the horizontal cutting plane; must be inside the object.")],
        keep: Annotated[Literal["both", "upper", "lower"], Field(description="Which halves to keep.")] = "both",
        place_on_cut_upper: Annotated[bool, Field(description="Lay the upper half on its cut face so it can be printed.")] = True,
    ) -> dict:
        """Cut an object with a horizontal plane (simple Z cut), without connectors. The halves become
        separate objects (undoable). Object ids change: read the returned list."""
        return await call("POST", f"objects/{object_id}/cut", {"z": z, "keep": keep, "place_on_cut_upper": place_on_cut_upper})
