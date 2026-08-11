# camera-gui — Buildroot package

A self-contained Buildroot package for the **camera-gui** application
(`AESDLinuxEgtProject`): an EGT + GStreamer camera GUI for the Microchip
SAMA7D65 Curiosity board.

The app receives an H.264/RTP video stream over UDP (port 5000), decodes it with
`avdec_h264`, and blits it onto a hardware overlay (HEO) plane on the LVDS panel.
A touch button captures a still JPEG to `~/camera-gui-capture` (created on
first use); each file is named `cap_<YYYYMMDD_HHMMSS>_<NNNNN>.jpg`. It installs
as `/usr/bin/AESDLinuxEgtProject`.

A background thread also listens on **UDP port 5001** for a "sensor" trigger:
any datagram received there shows **"Sensor Detected"** in the GUI below the
Capture button and fires the *same* capture-and-save action as pressing the
button. This lets an external sensor (a motion detector, a PIR, another host)
snap and store a frame automatically.

This directory is meant to be committed to git and later dropped into a Buildroot
`BR2_EXTERNAL` tree.

## Architecture

**Board / network topology** — point-to-point Ethernet on a private
`192.168.10.0/24` subnet (static IPs, no DHCP). The SAMA7D65 board is purely a
*receiver*: a source pushes RTP video to UDP 5000, any host may fire the sensor
trigger on UDP 5001, and the board drives the LVDS panel via the LCDC.

```
   +--------------------+   +-----------------------+   +------------------+
   | SAM9X75 camera     |   | Laptop / host PC      |   | Sensor / PIR /   |
   | 192.168.10.10      |   | webcam_stream.sh      |   | any host         |
   | RTP raw YCbCr 4:2:2|   | (/dev/video0 -> H.264)|   | (motion event)   |
   +---------+----------+   +-----------+-----------+   +--------+---------+
             | :5000                    | :5000                  | :5001
             +------------+-------------+------------------------+
                          |
                          v
      +=======================================================+
      |   Ethernet  192.168.10.0/24   (static IP, no DHCP)    |
      +============================+==========================+
                                   |
                                   v
                 +-------------------------------------+
                 | SAMA7D65 Curiosity board (receiver) |
                 | 192.168.10.22                       |
                 | runs AESDLinuxEgtProject            |
                 |   :5000  live RTP video  (in)       |
                 |   :5001  sensor trigger  (in)       |
                 +------------------+------------------+
                                    |  LCDC parallel-RGB
                                    v
                 +-------------------------------------+
                 | LVDS panel  800 x 480 (WVGA)        |
                 | [ live video 600px | panel 200px ]  |
                 +-------------------------------------+
```

**Software design (inside the process)** — one process owns everything. The
GStreamer pipeline fans out at a `tee` into a *display* branch (an `appsink` the
UI pump pulls from) and a *capture* branch (a normally-closed `valve` the UI
opens for one buffer). A single UI-thread pump (~30 Hz) does all frame-blitting,
bus-draining and widget work; the lone extra thread (UDP 5001) only sets an
atomic flag. Two hardware planes — video (HEO overlay) and UI (ARGB) — are
composited by the LCDC.

```
   UDP :5000 (RTP video)            UDP :5001 (sensor trigger)
        |                                   |
        v                                   v
  +-----------------------------+     +---------------+
  | GStreamer pipeline (C API)  |     | sensor thread |
  |  udpsrc                     |     |  recvfrom()   |
  |   -> jitterbuffer -> depay  |     |  sets atomic  |
  |   -> decode -> tee          |     +-------+-------+
  +--------------+--------------+             |
      DISPLAY    |    CAPTURE                 | atomic<bool>
      branch     |    branch                  | (lock-free
        v        |      v                     |  hand-off)
  queue->convert | queue->valve->jpegenc      |
    ->appsink    |   ->multifilesink          |
        |        |          |                 |
        | newest |          v                 |
        | frame  |   ~/camera-gui-capture/    |
        |        |        *.jpg               |
        v        v                            v
  +===========================================================+
  |         EGT PeriodicTimer  ~30 Hz   [ UI THREAD ]         |
  |-----------------------------------------------------------|
  | each tick:                                                |
  |  1. pull newest sample from appsink -> blit to HEO plane  |
  |  2. drain GStreamer bus -> status text ("Saved"/error)    |
  |  3. if sensor flag: show "Sensor Detected" + do_capture() |
  |       do_capture opens the valve for exactly ONE buffer;  |
  |       refused with "No camera feed" if no live frames     |
  +=============================+=============================+
                                | frame + widget draw
                                v
   +-----------------------+         +-----------------------------+
   | HEO video overlay     |         | UI overlay plane (ARGB8888) |
   | plane  600x480 xrgb   |         | Capture / status / sensor   |
   +-----------+-----------+         +--------------+--------------+
               |                                    |
               +----------------> LCDC <------------+
                    (composites both planes in hardware)
                                   |
                                   v
                          LVDS panel  800 x 480
```

## Contents

```
camera-gui/
├── README.md            This file
├── Config.in            Kconfig option (BR2_PACKAGE_CAMERA_GUI) + selects
├── camera-gui.mk        generic-package recipe (local source, cross build)
├── camera-gui.service   systemd unit (auto-starts the app after boot)
├── camera-gui-start.sh  Starter: stops egtdemo (if running) then launches app
├── defconfig.fragment   Lines to add to your board defconfig
└── src/
    ├── main.cpp         Application source
    └── Makefile         Cross/native build via pkg-config
```

## How to add it to a Buildroot external tree

1. **Copy the package** into your external tree's `package/` directory:

   ```sh
   cp -r camera-gui  <BR2_EXTERNAL>/package/camera-gui
   ```

2. **Fix the SITE path macro.** `camera-gui.mk` locates its local source via the
   external's path macro, which is derived from the `name:` field of your
   external's `external.desc` (macro form: `BR2_EXTERNAL_<NAME>_PATH`). The file
   ships with `MCHP`:

   ```make
   CAMERA_GUI_SITE = $(BR2_EXTERNAL_MCHP_PATH)/package/camera-gui/src
   ```

   If your external is *not* named `MCHP`, replace `MCHP` with your external's
   name. For example, for `name: ACME`:

   ```make
   CAMERA_GUI_SITE = $(BR2_EXTERNAL_ACME_PATH)/package/camera-gui/src
   ```

3. **Register the Kconfig option.** Add this line to your external's top-level
   `Config.in` (e.g. inside an application menu):

   ```
   source "$BR2_EXTERNAL_<NAME>_PATH/package/camera-gui/Config.in"
   ```

   (The `external.mk` `$(wildcard .../package/*/*.mk)` include picks up
   `camera-gui.mk` automatically — no change needed there.)

4. **Enable it in your defconfig.** Append the lines from `defconfig.fragment`
   to your board defconfig (see that file for details), or just enable
   `BR2_PACKAGE_CAMERA_GUI` in `make menuconfig` under your application menu.

## Dependencies

`Config.in` `select`s everything the app needs:

* `BR2_PACKAGE_EGT` — the EGT graphics library. With its default media support
  it transitively pulls in `gstreamer1`, `gst1-plugins-base` (incl. the `app`
  and `videoconvertscale` plugins), `gst1-plugins-good`, and `gst1-libav`
  (`avdec_h264`) — i.e. the whole decode/display stack the app links against and
  uses at runtime.
* The receive/capture pipeline additionally needs these `gst1-plugins-good`
  elements, which EGT does not guarantee, so the package selects them
  explicitly: `UDP`, `RTP`, `RTPMANAGER`, `MULTIFILE`.

The package also `depends on` a C++ toolchain (`BR2_INSTALL_LIBSTDCPP`,
`BR2_USE_WCHAR`, `BR2_TOOLCHAIN_GCC_AT_LEAST_9`), mirroring EGT's own
requirements (EGT is C++17).

The **sensor trigger** (UDP 5001) needs no extra Buildroot packages — it uses
plain POSIX UDP sockets and a `std::thread`, so the only build requirement is
`-pthread`, which `src/Makefile` already adds to the compile and link flags.

## Build

From your Buildroot output/top directory, after the steps above:

```sh
make <your_board>_defconfig     # picks up the package via BR2_EXTERNAL
make camera-gui                 # build just this package
# or: make                      # build the whole image
```

The binary is installed to `/usr/bin/AESDLinuxEgtProject` in the target rootfs.

### Standalone build (outside Buildroot)

`src/Makefile` also builds natively on any host that has the libraries and their
`.pc` files available via `pkg-config`:

```sh
cd src && make        # honours $(CXX) and $(PKG_CONFIG)
```

## Automatic startup (systemd service)

The package installs and enables a systemd service so the application starts
automatically after Linux finishes booting:

* `camera-gui.service` → `/usr/lib/systemd/system/camera-gui.service`, enabled
  via a `multi-user.target.wants` symlink. It is ordered `After=egtdemo.service`
  and runs the starter script.
* `camera-gui-start.sh` → `/usr/bin/camera-gui-start.sh`. It checks whether the
  **egtdemo** service (the EGT launcher demo) is running; if so it stops it to
  release the display, then `exec`s `/usr/bin/AESDLinuxEgtProject`.

The starter sets sensible, overridable runtime defaults
(`CAMERA_GUI_H264=1`, `CAMERA_GUI_LATENCY=400`, `GST_DEBUG=1`).

Manage it on the board with:

```sh
systemctl status camera-gui        # state / logs
systemctl restart camera-gui       # restart
systemctl disable camera-gui       # stop auto-start on boot
journalctl -u camera-gui -b        # boot log for the service
```

To change defaults without editing the script, add a drop-in, e.g.
`/etc/systemd/system/camera-gui.service.d/env.conf`:

```ini
[Service]
Environment=CAMERA_GUI_LATENCY=200
Environment=CAMERA_GUI_H264=1
```

## Run manually (on the board)

```sh
systemctl stop egtdemo    # free the display if the launcher demo is running
CAMERA_GUI_H264=1 CAMERA_GUI_LATENCY=400 AESDLinuxEgtProject
```

Pair it with the `webcam_stream.sh` sender on the host. See that script's PDF
reference for options.

## Sensor trigger (UDP 5001)

Besides the on-screen Capture button, the app runs a small background thread
that listens for UDP datagrams on **port 5001**. Any datagram (of any content —
the payload is only logged) does two things on the next UI tick:

1. shows **`Sensor Detected`** in yellow, below the Capture button, for ~3 s, and
2. triggers the **identical** capture/save path as the button — a timestamped
   JPEG is written to `~/camera-gui-capture` (including the same *No camera feed*
   guard: a datagram received with no live feed is refused, see below).

Thread-safety: the listener thread never touches EGT/GStreamer objects (they are
not thread-safe). It only sets an atomic flag; the existing 33 ms UI pump reads
that flag and performs both the label update and the capture on the UI thread,
so the whole app stays single-threaded with respect to EGT/GStreamer. The thread
uses a 1-second socket receive timeout so it exits cleanly at shutdown.

Test it from any host that can reach the board:

```sh
echo "detected" | nc -u -w1 192.168.10.22 5001    # BusyBox/Netcat
# or:
printf 'x' > /dev/udp/192.168.10.22/5001          # bash built-in, no nc needed
```

The port is a compile-time constant (`kSensorPort = 5001` in `main.cpp`); change
it there and rebuild if it clashes with something on your network.

## Capture status label & the "No camera feed" guard

The status label (top of the right column) shows a capture through a short,
auto-reverting sequence:

```
Live  ->  Capturing...  ->  Saved  --(~3 s)-->  Live
```

Notes on the design:

* **Short text only.** The label ever holds only `Live`, `Capturing...`, `Saved`
  or `No camera feed`, so it always fits the narrow (~180 px) control column and
  stays on screen. The full saved path is written to the log, not the label
  (showing the filename overflowed the label and drew off the screen edge).
* **Guaranteed revert.** The "return to `Live`" deadline is armed in
  `do_capture()` at capture time, so the label reverts even if the
  `multifilesink` "file written" message is missed.
* **No camera feed guard.** A capture only makes sense while a live feed is
  arriving. If no frame has been received recently (`kFeedStaleTicks`, ~1 s) —
  e.g. at startup before the first frame, or during a stream drop — both the
  button and the sensor trigger are refused: the label shows `No camera feed`
  for ~3 s, the pipeline is left untouched, and no empty/stale JPEG is written.

## Display-type check / auto-rotate

`check_screen_type.sh` (installed to `/usr/bin`) runs as a separate, non-fatal
`ExecStartPre` step of `camera-gui.service`. It inspects the connected DRM
connector:

* **LVDS** panel  -> prints `LVDS detected`, no change.
* **MIPI/DSI** panel -> prints `MIPI detected` and makes a best-effort 90°
  rotation via `modetest` (the Microchip LCDC may not support HW rotation, so
  this is attempted and logged, never fatal).

Disable it at runtime (no rebuild) by editing `/etc/default/camera-gui`:

```sh
SCREEN_CHECK=0          # skip the check/rotate entirely
SCREEN_ROTATE=90        # degrees to try on a MIPI/DSI panel
systemctl restart camera-gui
```

Run it standalone for diagnostics: `check_screen_type.sh`.
