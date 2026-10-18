"""MCP tools for the Orca Zoukei calibration group (G6): list Orca's built-in calibrations (the Calibration menu) and
start one with its parameters. They map onto the endpoints documented at the top of
src/slic3r/GUI/RemoteAPI/RemoteAPICalib.cpp.

Starting a calibration does what the menu dialog does on OK: it replaces the current project with the calibration project
(model + modified print / filament / printer settings of the SELECTED presets). Nothing is sliced or sent to a printer;
slice afterwards (slice_and_wait). Choose the printer / filament presets first."""
from __future__ import annotations

from typing import Annotated, Literal

from pydantic import Field

from .tools_common import call, clean

Kind = Literal["temperature", "max_volumetric_speed", "pressure_advance", "flow_rate", "retraction", "vfa", "cornering",
               "input_shaping_freq", "input_shaping_damp"]


def register(mcp) -> None:
    @mcp.tool()
    async def calibration_list() -> dict:
        """List Orca's built-in calibrations (temperature tower, max volumetric speed, pressure advance tower / line / pattern,
        flow rate, retraction, VFA, cornering, input shaping frequency / damping) with every parameter (type, default, limits)
        and the printer context (G-code flavor, belt printer, available shaper types)."""
        return await call("GET", "calibrations")

    @mcp.tool()
    async def calibration_start(
        kind: Annotated[Kind, Field(description="Which calibration (see calibration_list).")],
        discard_changes: Annotated[bool, Field(description="The project is replaced: refused (409 unsaved_changes) when it has unsaved changes unless True.")] = False,
        start: Annotated[float | None, Field(description="Start value: temperature C / flow mm3/s / PA / retraction mm / VFA mm/s / cornering.")] = None,
        end: Annotated[float | None, Field(description="End value (same unit as start).")] = None,
        step: Annotated[float | None, Field(description="Step (max_volumetric_speed, pressure_advance, retraction, vfa).")] = None,
        filament_type: Annotated[Literal["pla", "abs_asa", "petg", "pctg", "tpu", "pa_cf", "pet_cf", "custom"] | None, Field(description="temperature: sets the default start / end range.")] = None,
        auto_scale: Annotated[bool | None, Field(description="temperature / vfa: scale the tower to the nozzle diameter (default True).")] = None,
        method: Annotated[Literal["tower", "line", "pattern"] | None, Field(description="pressure_advance: test type (belt printers: tower only).")] = None,
        extruder_type: Annotated[Literal["dde", "bowden"] | None, Field(description="pressure_advance: only changes the defaults.")] = None,
        print_numbers: Annotated[bool | None, Field(description="pressure_advance line: print the PA value next to each line.")] = None,
        accelerations: Annotated[list[int] | None, Field(description="pressure_advance pattern: accelerations mm/s2 (must exceed all speeds).")] = None,
        speeds: Annotated[list[int] | None, Field(description="pressure_advance pattern: speeds mm/s.")] = None,
        flow_type: Annotated[Literal["pass1", "pass2", "yolo", "yolo_perfectionist"] | None, Field(description="flow_rate: which test (default yolo).")] = None,
        pattern: Annotated[Literal["archimedean", "monotonic"] | None, Field(description="flow_rate: top surface pattern.")] = None,
        layer_height: Annotated[float | None, Field(description="vfa: layer height mm (0 = automatic).")] = None,
        test_model: Annotated[int | None, Field(description="temperature (belt: 1 = overhang), cornering (0 ringing, 1 fast, 2 SCV-V2), input shaping (0 ringing tower, 1 fast tower).")] = None,
        freq_start_x: Annotated[float | None, Field(description="input_shaping_freq: start Hz, X.")] = None,
        freq_end_x: Annotated[float | None, Field(description="input_shaping_freq: end Hz, X.")] = None,
        freq_start_y: Annotated[float | None, Field(description="input_shaping_freq: start Hz, Y.")] = None,
        freq_end_y: Annotated[float | None, Field(description="input_shaping_freq: end Hz, Y.")] = None,
        damping: Annotated[float | None, Field(description="input_shaping_freq: damping factor 0..1.")] = None,
        freq_x: Annotated[float | None, Field(description="input_shaping_damp: fixed frequency Hz, X.")] = None,
        freq_y: Annotated[float | None, Field(description="input_shaping_damp: fixed frequency Hz, Y.")] = None,
        damping_start: Annotated[float | None, Field(description="input_shaping_damp: first damping factor.")] = None,
        damping_end: Annotated[float | None, Field(description="input_shaping_damp: last damping factor.")] = None,
        shaper_type: Annotated[str | None, Field(description="input shaping: shaper type (calibration_list shows the printer's types).")] = None,
    ) -> dict:
        """Create the calibration project like the Calibration menu does and report what was generated (objects, modified
        settings, requested values, warnings). Values left out use the dialog defaults. Replaces the current project. Then
        slice with slice_and_wait."""
        body = clean(discard_changes=True if discard_changes else None, start=start, end=end, step=step,
                     filament_type=filament_type, auto_scale=auto_scale, method=method, extruder_type=extruder_type,
                     print_numbers=print_numbers, accelerations=accelerations, speeds=speeds, type=flow_type,
                     pattern=pattern, layer_height=layer_height, test_model=test_model, freq_start_x=freq_start_x,
                     freq_end_x=freq_end_x, freq_start_y=freq_start_y, freq_end_y=freq_end_y, damping=damping,
                     freq_x=freq_x, freq_y=freq_y, damping_start=damping_start, damping_end=damping_end,
                     shaper_type=shaper_type)
        return await call("POST", f"calibrations/{kind}/start", body, timeout=180.0)
