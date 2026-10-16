# Creality device panel (embedded)

The compiled web panel in `assets/` and `img/` is taken from Creality Print
(https://github.com/CrealityOfficial/CrealityPrint), folder `resources/web/deviceMgr`,
commit `59ae8cbb0437dcc7236e27433ecd4aeb52be0244`. Creality Print is licensed under the
GNU Affero General Public License v3.0 (AGPL-3.0), the same license as OrcaSlicer.
Copyright (c) Creality and the Creality Print contributors.

`machine_caps.json` is derived from Creality Print's `resources/profiles/Creality/machine`
presets at the same commit (fan, LED, chamber-heater and bed-motion flags per printer preset).

`resources/cert/ca.crt` (Creality's printer CA, used to verify the printer's TLS
certificate) comes from the same commit.

Changes made for Orca Zoukei (AGPL-3.0):

- `index.html`: loads `orca-theme.css` and `orca-theme.js` before the bundle and adds a
  Content-Security-Policy that keeps scripts local.
- `orca-theme.css` / `orca-theme.js`: OrcaSlicer colours and font instead of Creality's
  palette, and a guard that keeps network requests on the LAN.
- `img/logo.png`: replaced by the Orca Zoukei icon.

The C++ host side (`src/slic3r/GUI/CrealityDevice/`, `src/slic3r/GUI/CrealityDevicePanel.*`)
ports the routes the panel needs from Creality Print's `src/slic3r/GUI/print_manage`
(DeviceMgrRoutes, SysRoutes, PrinterMgr, LanDeviceProbe, WebSocketProxy) at the same commit.
Cloud and account features are not ported.
