# camera-gui — Sprint 2: PIR / motion sensor over UDP :5001

Second milestone of the **camera-gui** project (`AESDLinuxEgtProject`). Builds
on Sprint 1 (EGT hello label + touch button) and adds a **PIR / motion-sensor
notifier** received over the network.

## What it does (new in Sprint 2)

* A background thread listens on **UDP port 5001** (`INADDR_ANY`).
* Any datagram received there is treated as a **motion / PIR detection**.
* The GUI shows **"Sensor Detected"** (yellow) below the button for ~3 seconds
  after each detection, then clears it.
* Everything from Sprint 1 (hello label, button + press counter) is retained.

Still **no** video and **no** GStreamer — that is Sprint 3, which also mirrors
the logs into **syslog**. Logging here is stderr-only.

## Design note — why a thread + a pump timer

EGT is **not** thread-safe. So the listener thread does the one thing that is
safe off the UI thread: block in `recvfrom()` and flip a `std::atomic<bool>`
flag. A UI-thread `PeriodicTimer` ("pump", ~30 Hz) polls that flag and performs
all widget updates. No cross-thread EGT access → no locking. This is the exact
pattern Sprint 3 reuses to drive the video pump.

```
   UDP :5001 datagram
        |
        v
  [sensor thread]  --recvfrom()--> set atomic detected=true
                                        |
                                        v (polled every 33ms)
                                   [UI pump timer] --> sensor.text("Sensor Detected")
```

## Files

```
camera-gui/
├── Config.in              # selects EGT; requires toolchain threads
├── camera-gui.mk          # generic-package (DEPENDENCIES = egt)
├── defconfig.fragment     # one line to enable the package
├── camera-gui.service     # systemd unit (After=network.target)
├── camera-gui-start.sh    # bring up eth0, stop egtdemo, launch
└── src/
    ├── main.cpp           # Sprint 1 UI + UDP :5001 sensor thread + pump timer
    └── Makefile           # pkg-config libegt, plus -pthread
```

## Build

Same as Sprint 1 — drop into `package/camera-gui/`, source the `Config.in`,
set `BR2_PACKAGE_CAMERA_GUI=y`, and `make`. The binary installs to
`/usr/bin/AESDLinuxEgtProject`.

## Test the sensor

From any host on the `192.168.10.0/24` network (the board is `192.168.10.22`):

```
echo motion | nc -u -w1 192.168.10.22 5001
```

The GUI should show **"Sensor Detected"** for a few seconds. On the target,
watch the log with:

```
/usr/bin/camera-gui-start.sh        # or: journalctl -u camera-gui -f
```
