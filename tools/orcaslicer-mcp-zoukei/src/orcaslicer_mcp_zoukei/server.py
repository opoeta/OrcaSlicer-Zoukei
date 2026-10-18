"""Orca Zoukei MCP bridge: the stock orcaslicer-mcp server plus the Zoukei tools.

The stock package (MaxEllis, AGPL-3.0, https://github.com/maxellis/orcaslicer-mcp) is a dependency, not
a copy: its `mcp` server object is imported and our tools are registered on it, so every stock tool keeps
working unchanged and the stock package can be upgraded on its own.
"""
from __future__ import annotations

from orcaslicer_mcp.server import main as _stock_main
from orcaslicer_mcp.server import mcp

from . import tools_calib, tools_color, tools_devices, tools_layers, tools_paint, tools_project, tools_support

tools_color.register(mcp)
tools_support.register(mcp)   # G1
tools_project.register(mcp)   # G2
tools_paint.register(mcp)     # G3
tools_layers.register(mcp)    # G4
tools_devices.register(mcp)   # G5 (control tools need the Preferences switch + confirm)
tools_calib.register(mcp)     # G6

try:  # later groups (supports, project ops, painting, per-layer, devices, calibration)
    from . import tools_more
    tools_more.register(mcp)
except ImportError:  # pragma: no cover - module absent in a partial checkout
    pass


def main() -> None:
    _stock_main()


if __name__ == "__main__":
    main()
