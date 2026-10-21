# BMCU firmware flasher

Device > Firmware shows a BMCU (open-source AMS lite clone for the Bambu A1 / A1 mini, serial starting with `0EA`) as
`BMCU-n` with its version and the GitHub status. The **Update firmware... / Reflash firmware...** button opens the
built-in flasher (`src/slic3r/GUI/Zoukei/BmcuFlasherDialog.cpp`).

## What it does

1. Lists the firmware of [jarczakpawel/BMCU-C-PJARCZAK](https://github.com/jarczakpawel/BMCU-C-PJARCZAK): the latest
   GitHub release (default), the `main` branch (what BMCU-Flasher downloads), older releases that still have
   `firmwares/manifest.txt` (V10.x and later), or a local `.bin`.
2. The variant (load force, slot, retraction, AUTOLOAD, RGB) is picked from the options the manifest of that version
   actually has.
3. Downloads `firmwares/<variant>` from `raw.githubusercontent.com` and checks size, CRC32 and SHA-256 against the
   manifest; verified files are cached in `<data_dir>/bmcu_firmware/<sha256>.bin`.
4. After an explicit confirmation (and only while the printer is not printing or updating), writes it with the WCH ISP
   protocol of the CH32V203: USB mode (CH340 1A86:7523 on the BMCU board, boot mode through DTR/RTS "AutoDI") or TTL mode
   (external USB-serial adapter, BOOT button by hand, automatic retry at 115200 baud if the fast baud fails).
   Every block is verified after writing (except the last, padded one, as in BMCU-Flasher), then the BMCU restarts.

Native USB (Toaiot boards without CH340, WCH ISP device 4348:55E0) is not supported: it needs libusb or the WCH CH375
driver DLL. Use BMCU-Flasher for those boards. Klipper firmware (BMCU-Klipper) is not offered either.

No Remote API / MCP endpoint can write BMCU firmware.

## Code

- `src/slic3r/Utils/BmcuFlasher.{hpp,cpp}`: packets, key derivation, flash sequence, hashes, manifest / releases parsing.
  No wx / network dependency; the serial port is an injected `Transport`.
- `src/slic3r/Utils/BmcuSerialPort.{hpp,cpp}`: the real serial port (Win32 / POSIX) and the port list.
- `tests/slic3rutils/test_bmcu_flasher.cpp`: golden packets, the frame reader, hashes, the recorded manifest and releases
  JSON (`tests/data/bmcu/`), and the whole sequence against a mock CH32V203 bootloader (USB wiring variants, protected
  flash, TTL fallback, cancel, wrong chip, verify failure).

## License of the ported code

The flash sequence, packet layout, key derivation, AutoDI search, adapter list and firmware catalogue format come from
[BMCU-Flasher](https://github.com/jarczakpawel/BMCU-Flasher) (`bmcu_flasher.py` v1.4), used under the MIT License:

```
MIT License

Copyright (c) 2026 Paweł Jarczak

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The firmware itself is not shipped with Orca: it is downloaded from its own repository when the user asks for it.
