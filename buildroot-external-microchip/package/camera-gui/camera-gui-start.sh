#!/bin/sh
#
# camera-gui-start.sh -- startup wrapper for the camera-gui application.
# (Sprint 1: no network, no GStreamer -- just take over the display and run.)
#
# Called by camera-gui.service after boot. The EGT launcher demo (egtdemo)
# owns the DRI display planes / LVDS panel; if it is running we must stop it
# first so camera-gui can take over the display, then launch the application.
#
set -u

APP=/usr/bin/AESDLinuxEgtProject
EGTDEMO=egtdemo.service

# If egtdemo is active it is holding the display (DRM master + LCDC planes);
# stop it AND wait for the display to be fully released before we launch.
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

echo "camera-gui: launching $APP"
exec "$APP"
