# camera-gui — Sprint 1: Hello text + simple button UI

The first milestone of the **camera-gui** project (`AESDLinuxEgtProject`): a
self-contained Buildroot external package that cross-compiles an **EGT**
(Ensemble Graphics Toolkit) application for the Microchip **SAMA7D65 Curiosity**
board and draws a minimal UI on the LVDS panel.

## What it does

* Shows a **"Hello, camera-gui!"** text label.
* Shows one touch **button**; each press updates an on-screen counter label.
* Logs every stage to **stderr** (serial console).

That's the whole scope. There is **no** video, **no** network and **no**
GStreamer yet — those are added in later sprints:

| Sprint | Adds |
|--------|------|
| 1 (this) | EGT app: hello label + button |
| 2 | UDP `:5001` PIR/motion sensor listener → "Sensor Detected" in the UI |
| 3 | GStreamer RTP video overlay + JPEG capture + syslog logging |

## Files

```
camera-gui/
├── Config.in              # BR2_PACKAGE_CAMERA_GUI (selects EGT)
├── camera-gui.mk          # generic-package recipe (DEPENDENCIES = egt)
├── defconfig.fragment     # one line to enable the package
├── camera-gui.service     # systemd unit (stops egtdemo, runs the start script)
├── camera-gui-start.sh    # release the display from egtdemo, then launch
└── src/
    ├── main.cpp           # EGT hello + button
    └── Makefile           # pkg-config libegt
```

## Build (inside a Buildroot BR2_EXTERNAL tree)

1. Drop this `camera-gui/` directory into your external tree under
   `package/camera-gui/`.
2. Reference it from the external `Config.in`:
   ```
   source "$BR2_EXTERNAL_MCHP_PATH/package/camera-gui/Config.in"
   ```
3. Enable it in your board defconfig (see `defconfig.fragment`):
   ```
   BR2_PACKAGE_CAMERA_GUI=y
   ```
4. Build:
   ```
   make            # or: make camera-gui-rebuild
   ```
   The binary installs to `/usr/bin/AESDLinuxEgtProject`.

## Run on the target

The systemd service starts automatically at boot. To run by hand:

```
systemctl stop camera-gui        # if the service grabbed the display
/usr/bin/camera-gui-start.sh      # stops egtdemo, then launches the app
```

## Standalone host build (optional)

With `libegt` available to `pkg-config`:

```
cd src && make
```
