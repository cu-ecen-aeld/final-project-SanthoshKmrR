#!/bin/sh
#
# camera-gui-start.sh -- startup wrapper for the camera-gui application.
# (Sprint 2: bring the network up so UDP :5001 sensor datagrams can arrive,
#  then take over the display and run. Still no GStreamer.)
#
set -u

APP=/usr/bin/AESDLinuxEgtProject
EGTDEMO=egtdemo.service

# Bring up the wired interface with the board's static IP so it can receive the
# UDP sensor datagrams on port 5001.
echo "camera-gui: configuring eth0 (192.168.10.22/255.0.0.0)"
ifconfig eth0 192.168.10.22 netmask 255.0.0.0 up

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

echo "camera-gui: launching $APP (listening for sensor on UDP :5001)"
exec "$APP"
