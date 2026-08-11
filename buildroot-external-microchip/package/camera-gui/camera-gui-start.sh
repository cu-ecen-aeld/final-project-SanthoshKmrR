#!/bin/sh
#
# camera-gui-start.sh -- startup wrapper for the camera-gui application.
#
# Called by camera-gui.service after boot. The EGT launcher demo (egtdemo)
# owns the DRI display planes / LVDS panel; if it is running we must stop it
# first so camera-gui can take over the display, then launch the application.
#
set -u

APP=/usr/bin/AESDLinuxEgtProject
EGTDEMO=egtdemo.service

# Recommended runtime defaults (overridable via the service Environment= or an
# environment file). H.264/RTP receive mode, larger jitter buffer for lossy
# links, and a quiet GStreamer/libav log.
export CAMERA_GUI_H264="${CAMERA_GUI_H264:-1}"
export CAMERA_GUI_LATENCY="${CAMERA_GUI_LATENCY:-250}"
export GST_DEBUG="${GST_DEBUG:-1}"

# Bring up the wired interface with the board's static IP so it can receive the
# RTP/UDP video stream from the sender.
echo "camera-gui: configuring eth0 (192.168.10.22/255.0.0.0)"
ifconfig eth0 192.168.10.22 netmask 255.0.0.0 up

# If egtdemo is active it is holding the display (DRM master + LCDC overlay/HEO
# planes); stop it AND wait for the display to be fully released before we
# launch. `systemctl stop` returns when the process exits, but the kernel drops
# the DRM master and tears down the LCDC planes slightly later -- if our EGT app
# opens the display in that gap it cannot become master / grab the HEO overlay
# and the video plane stays black (no visible feed). So we poll until the unit
# is inactive, then give the display a short settle.
if systemctl is-active --quiet "$EGTDEMO"; then
    echo "camera-gui: $EGTDEMO is active -- stopping it before launch"
    systemctl stop "$EGTDEMO"
    i=0
    while systemctl is-active --quiet "$EGTDEMO" && [ "$i" -lt 50 ]; do
        sleep 0.1
        i=$((i + 1))
    done
    # Let the kernel finish releasing the DRM master / LCDC planes.
    sleep 0.5
    echo "camera-gui: $EGTDEMO stopped -- display released"
else
    echo "camera-gui: $EGTDEMO not active -- nothing to stop"
fi

echo "camera-gui: launching $APP (H264=$CAMERA_GUI_H264 LATENCY=$CAMERA_GUI_LATENCY)"
exec "$APP"
