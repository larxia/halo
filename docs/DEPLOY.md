# HALO — build, install, back up, recover

> **Status: no HALO build has run on an FM-1 yet.** The installation route below is the established one that every
> fm1-base firmware uses, and those firmwares (CTL-1, Radio and others) run on FM-1s today. But HALO's first install
> is also its first hardware test. Install it on an FM-1 whose data you have backed up, and keep the way back to
> official firmware at hand.

## 1. Build

HALO needs JieLi's Linux toolchain (clang 4.0.1 for pi32v2) and three files of JieLi's AC79 SDK. Neither could be
downloaded in the environment where HALO was written, so it was built and tested there only as the host simulator.
Two ways to build the device package:

**GitHub Actions (recommended, the same route as your other FM-1 firmwares):** push this repository to GitHub.
`.github/workflows/build.yml` runs the shared, reviewed FM-1 builder (`zvenson/fm1-build@v1`). It installs the
toolchain from JieLi's server and the SDK from JieLi's repository, builds, runs the simulator tests, and attests the
package. A push to `main` leaves `halo.fwsc` as the run's artifact. A tag `v0.1` makes a draft release.

**On a Linux x86-64 machine:**

```
git clone --recursive <your repository>
cd <repository>
base/tools/get_toolchain.sh                      # JieLi's toolchain to ~/.jieli
base/bsp/tools/get_sdk.sh                        # the AC79 SDK files (checked by hash)
pip install Pillow fonttools
./build.sh                                       # firmware/build/halo.fwsc
firmware/tests/run_tests.sh                      # the simulator tests
```

macOS works through Docker (see `base/bsp` and Felucca's BUILDING.md).

## 2. Back up before installing

Changing firmware does not carry data across, so save what is on the FM-1 now.

* **On M-VAVE's stock firmware:** export your presets with M-VAVE's own tools.
* **On FM1 Open:** in the FM1 Open workbench, export the projects, read and export voice groups A–D separately, and
  back up the sample bank (project files do not contain sample audio).
* **On another community firmware (your VOID or HYPER, or others):** use that firmware's own backup, if it has one.
* **On HALO (later updates):** `python3 base/tools/fm1_backup.py backup halo.halo` saves the sound, song and settings
  over USB. Restoring is all or nothing. The browser page `base/web/backup/` does the same.

## 3. Install

1. Charge the FM-1, or keep it on USB power. Use a USB **data** cable, directly into the computer, not through a hub.
2. Close any app that has the FM-1's MIDI port open (a DAW, the M-VAVE app, another installer's tab).
3. Install, either way:
   * **Command line:** `pip install mido python-rtmidi`, then
     `python3 base/tools/fm1_install.py firmware/build/halo.fwsc`. It finds the FM-1, shows what it will write, and
     asks first.
   * **Browser:** the installer page fm1-base generates for a firmware (`base/tools/make_site.py`, from
     `base/web/installer.html`), in Chrome or Edge.
4. Do not unplug during the write. The installer verifies the installed identity (`FM-1_74...`) after the restart.
5. If an install is cut off, the FM-1 stays in update mode. Run the installer again and it finishes the write.

## 4. Going back

* **To official firmware:** the installer's "back to official" (the fm1-base and Felucca installers both have it),
  M-VAVE's own updater, or the FM1 Open workbench's restore to stock V15.
* **To another firmware:** install its package the same way. From one custom firmware to another, going through
  official firmware first is the safer path. Some installers warn about this.

## 5. Recovery, if the FM-1 will not start

1. **Safe mode:** fm1-base counts failed starts. After two, it starts in safe mode, where the update entry still
   works. Install again, or go back to official firmware.
2. **Boot mode:** if the FM-1 stays in the chip's update (boot) mode, the installer can still write to it.
   `base/bsp/tools/fm1_rescue.sh` (macOS) writes stock firmware back through that mode.
3. **Last resort:** FM-1 Transporter (Felucca wiki) reads and writes the flash through a Seeed XIAO RP2040 wired to
   the FM-1's USB lines. Back up the flash first, then write M-VAVE's `FM-1.fwsc`. This means opening the unit.

The reverse-engineering project (AL-255/FM-1-RE) notes that the update protocol is not a proven recovery mechanism:
an interrupted write has no guaranteed rollback. Keep power stable during installs.

## 6. Identity and naming

HALO identifies itself as `FM-1_74` (development builds `FM-1_7400`, releases `FM-1_74XXYYZZ`; set in
`firmware/fm1.toml`), so installers and firmware hubs can tell it apart from your VOID and HYPER. If either of those
uses `FM-1_74`, change `id_prefix` before the first build.
