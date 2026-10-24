"""MCP tools for the Orca Zoukei printers and printing group (G5). They map onto the endpoints documented at the top
of src/slic3r/GUI/RemoteAPI/RemoteAPIDevices.cpp.

Reads are always allowed. Every control tool (print_send, print_pause / resume / stop, device_clear_error, printer_set_temperature,
filament_load / unload, slot_write) is refused by OrcaSlicer (HTTP 403, blocked_by_remote_api_policy) unless BOTH hold:
  1. the owner ticked Preferences > Remote API > "Allow printer control from the Remote API" (default OFF; it cannot be
     changed through the API), and
  2. the call passes confirm=True (ask the owner first, then send it).
Each control call is logged by OrcaSlicer and answers with what was sent. The printers' own safety rules stay in force
(official Snapmaker spools are never forced, slot writes and filament moves are refused while a print runs, ...)."""
from __future__ import annotations

from typing import Annotated
from urllib.parse import quote

from pydantic import Field

from .tools_common import call, clean

Device = Annotated[str, Field(description="Device id from device_list, or 'selected' (default) for the printer selected in OrcaSlicer.")]
Confirm = Annotated[bool, Field(description="Must be True for the call to be accepted. Also needs Preferences > Remote API > 'Allow printer control from the Remote API' (default OFF). Only set it after the owner agreed to this exact action.")]
Unit = Annotated[int | None, Field(description="Unit id from device_slots (AMS / CFS box id; 254 / 255 = external spool). Not used by Snapmaker U1.")]
Slot = Annotated[int, Field(description="0-based slot from device_slots (Snapmaker U1: head 0..3; Flashforge IFS: 0..3).")]


def _dev(device: str) -> str:
    return quote(device or "selected", safe=":")


def _body(confirm: bool, **kw) -> dict:
    body = clean(**kw)
    if confirm:
        body["confirm"] = True
    return body


def register(mcp) -> None:
    # ---- read-only ----
    @mcp.tool()
    async def device_list() -> dict:
        """Printers OrcaSlicer knows (device manager): id, name, model, agent (bbl, creality_cfs, flashforge, snapmaker, ...),
        online, connected, selected, state. Also says whether printer control is enabled in Preferences."""
        return await call("GET", "devices")

    @mcp.tool()
    async def device_status(device: Device = "selected") -> dict:
        """Status of a printer: state, progress %, layer / total layers, remaining seconds, job, nozzle / bed / chamber
        temperatures, what it can do now (pause / resume / stop), and its filament slots (with agent details such as the
        Snapmaker U1 head lock or the Flashforge IFS mode)."""
        return await call("GET", f"devices/{_dev(device)}")

    @mcp.tool()
    async def device_slots(device: Device = "selected") -> dict:
        """Filament slots of a printer: Bambu AMS, Creality CFS, Flashforge IFS, Snapmaker U1 heads and the external spool.
        Each slot has colour(s), type, remaining %, filament ids; U1 heads also report the official-spool lock
        (official_locked) and the feeder state."""
        return await call("GET", f"devices/{_dev(device)}/slots")

    @mcp.tool()
    async def device_activity(
        device: Device = "selected",
        since: Annotated[int, Field(description="Return only entries with seq > since. Pass the 'next' value of the previous answer to follow the log (poll).", ge=0)] = 0,
        limit: Annotated[int, Field(description="Max entries (1..2000, default 200). With since=0 the newest entries are returned.", ge=1, le=2000)] = 200,
        source: Annotated[str | None, Field(description="Comma-separated filter: ws-out, ws-in, sync, agent, error, klipper.")] = None,
        q: Annotated[str | None, Field(description="Case-insensitive text filter on the entry text.")] = None,
        raw: Annotated[bool, Field(description="Include the raw (redacted) JSON of each entry.")] = False,
    ) -> dict:
        """Live activity log of a printer in OrcaSlicer (the 'Console' of the device panel): websocket messages sent and
        received (changes only, heartbeats and telemetry skipped), project -> CFS / IFS / U1 sync decisions (which project
        filament, target slot, why, wrote or skipped), agent events and errors. Read-only; tokens, passwords and cookies are
        never recorded. Answer: {entries:[{seq,t,source,text,raw?}], next, dropped, total}."""
        return await call("GET", f"devices/{_dev(device)}/activity",
                          params=clean(since=since or None, limit=limit, source=source, q=q, raw=1 if raw else None))

    @mcp.tool()
    async def print_hosts() -> dict:
        """Print host presets (OctoPrint, Moonraker, PrusaLink, Duet, ...) of the printer presets and physical printers.
        Addresses and kinds only; API keys and passwords are never returned."""
        return await call("GET", "print_hosts")

    @mcp.tool()
    async def printer_control_status() -> dict:
        """Whether Preferences > Remote API > 'Allow printer control from the Remote API' is on. The control tools only work
        when it is on AND confirm=True is passed."""
        return await call("GET", "printer_control")

    # ---- control (gated) ----
    @mcp.tool()
    async def print_send(
        confirm: Confirm = False,
        name: Annotated[str | None, Field(description="File name on the printer (plain name, '.gcode' is added). Default: the project name.")] = None,
        plate: Annotated[int | None, Field(description="0-based plate; it must be sliced. Default: the current plate.")] = None,
        start: Annotated[bool, Field(description="Start the print after the upload (only if the print host supports it). Default: upload only.")] = False,
        storage: Annotated[str | None, Field(description="Target storage of hosts that have several (PrusaLink).")] = None,
        group: Annotated[str | None, Field(description="Target group of hosts that have them (Repetier).")] = None,
    ) -> dict:
        """CONTROL. Upload the sliced plate to the print host of the SELECTED printer preset (OctoPrint, Moonraker, PrusaLink ...)
        through the app's upload queue, optionally starting the print. Printers that expect a .gcode.3mf or are sent from the
        window's device dialog are not supported here. Needs the Preferences switch and confirm=True."""
        return await call("POST", "print/send", _body(confirm, name=name, plate=plate, start=True if start else None,
                                                      storage=storage, group=group), timeout=180.0)

    @mcp.tool()
    async def print_pause(device: Device = "selected", confirm: Confirm = False) -> dict:
        """CONTROL. Pause the running print of a printer. Needs the Preferences switch and confirm=True."""
        return await call("POST", f"devices/{_dev(device)}/pause", _body(confirm))

    @mcp.tool()
    async def print_resume(device: Device = "selected", confirm: Confirm = False) -> dict:
        """CONTROL. Resume a paused print. Needs the Preferences switch and confirm=True."""
        return await call("POST", f"devices/{_dev(device)}/resume", _body(confirm))

    @mcp.tool()
    async def print_stop(device: Device = "selected", confirm: Confirm = False) -> dict:
        """CONTROL, DESTRUCTIVE. Stop (cancel) the running or paused print; it cannot be resumed. Needs the Preferences
        switch and confirm=True."""
        return await call("POST", f"devices/{_dev(device)}/stop", _body(confirm))

    @mcp.tool()
    async def device_clear_error(device: Device = "selected", confirm: Confirm = False) -> dict:
        """CONTROL. Creality printers only (K2 / K2 Plus ...): dismiss the active printer error, like the OK button of Creality
        Print (websocket {"method":"set","params":{"cleanErr":1}}). Fix the cause first; the error disappears from the status once
        the printer reports errcode 0. Needs the Preferences switch and confirm=True."""
        return await call("POST", f"devices/{_dev(device)}/clear_error", _body(confirm))

    @mcp.tool()
    async def printer_set_temperature(
        device: Device = "selected",
        nozzle: Annotated[int | None, Field(description="Nozzle target in C, 0..350 (0 = off).")] = None,
        bed: Annotated[int | None, Field(description="Bed target in C, 0..printer limit (120 if unknown).")] = None,
        chamber: Annotated[int | None, Field(description="Chamber target in C, 0..80.")] = None,
        extruder: Annotated[int, Field(description="Extruder index for the nozzle target (multi-extruder printers).")] = 0,
        confirm: Confirm = False,
    ) -> dict:
        """CONTROL. Set nozzle / bed / chamber target temperatures through the app's printer commands. Needs the Preferences
        switch and confirm=True."""
        return await call("POST", f"devices/{_dev(device)}/temperature",
                          _body(confirm, nozzle=nozzle, bed=bed, chamber=chamber, extruder=extruder if nozzle is not None else None))

    async def _feed(action: str, device: str, slot: int, unit: int | None, nozzle_temp: int | None, confirm: bool) -> dict:
        return await call("POST", f"devices/{_dev(device)}/filament",
                          _body(confirm, action=action, slot=slot, unit=unit, nozzle_temp=nozzle_temp))

    @mcp.tool()
    async def filament_load(
        slot: Slot,
        device: Device = "selected",
        unit: Unit = None,
        nozzle_temp: Annotated[int | None, Field(description="Nozzle temperature for the move, 150..320 C (default 210).")] = None,
        confirm: Confirm = False,
    ) -> dict:
        """CONTROL. Load the filament of a slot into the extruder (heats the nozzle and moves filament). Refused while a
        print runs. Needs the Preferences switch and confirm=True."""
        return await _feed("load", device, slot, unit, nozzle_temp, confirm)

    @mcp.tool()
    async def filament_unload(
        slot: Slot,
        device: Device = "selected",
        unit: Unit = None,
        nozzle_temp: Annotated[int | None, Field(description="Nozzle temperature for the move, 150..320 C (default 210).")] = None,
        confirm: Confirm = False,
    ) -> dict:
        """CONTROL. Unload the filament from the extruder (heats the nozzle and moves filament). Refused while a print runs.
        Needs the Preferences switch and confirm=True."""
        return await _feed("unload", device, slot, unit, nozzle_temp, confirm)

    @mcp.tool()
    async def slot_write(
        slot: Slot,
        device: Device = "selected",
        unit: Unit = None,
        type: Annotated[str | None, Field(description="Filament type to write, e.g. 'PLA', 'PETG', 'PLA SILK'.")] = None,
        color: Annotated[str | None, Field(description="'#RRGGBB' colour to write.")] = None,
        colors: Annotated[list[str] | None, Field(description="2..4 colours '#RRGGBB' for dual / tri / gradient filaments (Snapmaker, Flashforge).")] = None,
        filament_id: Annotated[str | None, Field(description="Bambu-style tray_info_idx (default: the slot's current one).")] = None,
        setting_id: Annotated[str | None, Field(description="Bambu-style setting_id (default: the slot's current one).")] = None,
        confirm: Confirm = False,
    ) -> dict:
        """CONTROL. Write a slot's colour and/or type to the printer (Bambu AMS, Creality CFS, Flashforge IFS on ZMOD, Snapmaker
        U1 head). Official Snapmaker spools are locked and are never forced; writes are refused while a print runs. Needs the
        Preferences switch and confirm=True."""
        return await call("PUT", f"devices/{_dev(device)}/slots",
                          _body(confirm, slot=slot, unit=unit, type=type, color=color, colors=colors,
                                filament_id=filament_id, setting_id=setting_id))

    # ---- Creality Cloud (filament presets to the account a K2 / K2 Plus is bound to) ----
    @mcp.tool()
    async def creality_cloud_status() -> dict:
        """Whether a Creality Cloud account is logged in in OrcaSlicer, its region (global / china) and whether saved filament
        presets are synced automatically. The login itself only happens in OrcaSlicer (the owner signs in on Creality's page);
        the token is never returned."""
        return await call("GET", "creality_cloud")

    @mcp.tool()
    async def creality_cloud_filaments() -> dict:
        """The owner's own filament presets with their Creality Cloud state (uploaded, changed since), the Creality printer
        presets a filament can be linked to (printer_targets) and the default one."""
        return await call("GET", "creality_cloud/filaments")

    @mcp.tool()
    async def creality_cloud_upload(
        presets: Annotated[list[str], Field(description="Names of the owner's filament presets (from creality_cloud_filaments).")],
        printer: Annotated[str | None, Field(description="Creality printer preset to link them to (printer_targets); default: the selected Creality printer.")] = None,
        dry_run: Annotated[bool, Field(description="True (default): only report what would be sent (create / update, printer, bytes); nothing leaves the computer.")] = True,
        confirm: Confirm = False,
        wait: Annotated[bool, Field(description="Block until every preset is done. Default False: the upload runs in the background (one preset every few seconds, waiting out the Creality rate limit); follow it with creality_cloud_upload_status.")] = False,
    ) -> dict:
        """Upload filament presets to the logged in Creality Cloud account, so a printer bound to it (K2 / K2 Plus with CFS)
        lists them among its materials. With dry_run=True (default) nothing is sent. A real upload (dry_run=False) is CONTROL:
        it needs Preferences > Remote API > 'Allow printer control from the Remote API' and confirm=True. It starts in the
        background and is paced (a few seconds between presets, backoff on the Creality rate limit)."""
        body = clean(presets=presets, printer=printer)
        if dry_run:
            body["dry_run"] = True
        elif confirm:
            body["confirm"] = True
            if wait:
                body["wait"] = True
        return await call("POST", "creality_cloud/upload", body)

    @mcp.tool()
    async def creality_cloud_upload_status() -> dict:
        """Progress of the Creality Cloud upload started through the API: state (idle | running | done | cancelled), the preset
        being sent (index / total, attempt, seconds waited for the rate limit) and the per-preset results."""
        return await call("GET", "creality_cloud/upload_status")

    @mcp.tool()
    async def creality_cloud_upload_cancel() -> dict:
        """Stop the running Creality Cloud upload after the current preset (the rest stays unsent)."""
        return await call("POST", "creality_cloud/upload_cancel")
