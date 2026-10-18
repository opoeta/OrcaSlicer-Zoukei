# tools/zoukei

## Mock Snapmaker U1 (`mock_u1_moonraker.py`)

A fake U1 (Klipper + Moonraker) for testing the U1 integration without the printer. Standard library only; it
binds to `127.0.0.1` and never talks to anything else.

```
python tools/zoukei/mock_u1_moonraker.py                    # port 7125, 4 heads, head 1 official (NFC, locked), head 4 empty
python tools/zoukei/mock_u1_moonraker.py --printing         # a print is running: every filament / map write must be refused
python tools/zoukei/mock_u1_moonraker.py --official 0,1 --empty 3 --port 7126 --fast
python tools/zoukei/test_mock_u1.py                         # checks the firmware rules the mock emulates
```

It answers `/printer/objects/query` (`print_task_config`, `filament_detect`, `filament_feed left|right`, `print_stats`,
`extruder`, `extruder1..3`, `heater_bed`, ...), `/printer/gcode/script`, `/server/*`, `/printer/print/*` and `/websocket`.
Every G-code script Orca sends is printed, kept in memory (`GET /mock/scripts`) and appended to
`mock_u1_scripts.jsonl` (`--record ""` turns that off). `GET /mock/state` shows the whole state,
`POST /mock/set {"print_state": "printing"}` flips the printer to printing and back to `"standby"`.

Pointing Orca Zoukei at it:

1. Preferences: turn on **Use printer agents instead of print hosts**.
2. Select a **Snapmaker U1** printer profile (any nozzle), open **Physical printer** and create one with host
   `127.0.0.1:7125` (host type OctoPrint / Moonraker as the profile has it; no API key). Select it.
3. The **Device** tab becomes the U1 screen (4 heads, official lock, feed state, temperatures); the sidebar shows
   the head badges (E1..E4). Edit a head, load / unload, or send a print and watch the console of the mock: the
   expected lines are `SET_PRINT_FILAMENT_CONFIG ...` (with `FORCE=1` only after the confirm dialog for the official
   head), `SET_PRINT_EXTRUDER_MAP ...` + `SET_PRINT_USED_EXTRUDERS ...` right before the file is uploaded,
   and `AUTO_FEEDING EXTRUDER=n LOAD=1|UNLOAD=1` only after a click on Load / Unload.
4. With `--printing` every one of those writes must be refused by the page / agent before it reaches the mock
   (the mock itself also answers them with the firmware's own refusal text).

The mock is built from the open firmware (github.com/Snapmaker/u1-klipper: `print_task_config.py`,
`filament_feed.py`, `lava/fluidd.cfg`), not from a real printer: the real U1 may differ in details.
