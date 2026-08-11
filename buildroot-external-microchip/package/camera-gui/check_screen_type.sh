#!/bin/sh
#
# check_screen_type.sh -- detect the attached display panel and, for a MIPI/DSI
# panel, best-effort rotate the screen 90 degrees before camera-gui launches.
#
# Behaviour:
#   * LVDS panel connected  -> print "LVDS detected" and exit (no rotation).
#   * MIPI/DSI panel         -> print "MIPI detected" and TRY to rotate the
#                               display 90 degrees (see rotate_90 below).
#   * nothing / unknown      -> log and exit 0 (never block the app).
#
# It is wired into camera-gui.service as a separate, non-fatal ExecStartPre step
# (the leading '-' in the unit means a failure here never stops the app). It is
# DISABLE-ABLE at runtime, no rebuild required: set SCREEN_CHECK=0 in
# /etc/default/camera-gui (or export SCREEN_CHECK=0 / CAMERA_GUI_SCREEN_CHECK=0).
#
# This script is intentionally standalone: it can be run by hand for diagnostics
#   check_screen_type.sh
# and it makes no changes on an LVDS panel.

set -u

TAG="check_screen_type"
DEFAULTS=/etc/default/camera-gui

log() { echo "$TAG: $*"; }

# ---- Runtime enable/disable -------------------------------------------------
# Pull in the defaults file when present (so manual runs honour the same toggle
# the systemd unit uses via EnvironmentFile). Values already in the environment
# take precedence.
if [ -r "$DEFAULTS" ]; then
    # shellcheck disable=SC1090
    . "$DEFAULTS"
fi

# Accept either name; SCREEN_CHECK is the documented one.
CHECK="${SCREEN_CHECK:-${CAMERA_GUI_SCREEN_CHECK:-1}}"
ROTATE_DEG="${SCREEN_ROTATE:-90}"

case "$CHECK" in
    0|no|NO|false|FALSE|off|OFF)
        log "screen check disabled (SCREEN_CHECK=$CHECK) -- skipping"
        exit 0
        ;;
esac

# ---- Panel detection --------------------------------------------------------
# Walk the DRM connectors exposed under sysfs and pick the first CONNECTED one.
# Connector directory names look like: card0-LVDS-1, card0-DSI-1, card0-DPI-1 ...
detect_connector() {
    for status in /sys/class/drm/*/status; do
        [ -r "$status" ] || continue
        read -r st < "$status" 2>/dev/null || continue
        [ "$st" = "connected" ] || continue
        conn=${status%/status}
        conn=${conn##*/}
        echo "$conn"
        return 0
    done
    return 1
}

# ---- Best-effort 90-degree rotation for MIPI/DSI panels ---------------------
# The Microchip LCDC (atmel-hlcdc) may not support hardware plane rotation, so
# this is explicitly best-effort: we try to set the DRM 'rotation' property on a
# plane via modetest and report the result. A failure is logged, not fatal --
# the display simply stays in its native orientation.
rotate_90() {
    if ! command -v modetest >/dev/null 2>&1; then
        log "modetest not available -- cannot rotate; leaving native orientation"
        return 1
    fi

    # 'rotate-90' is bit 1 of the DRM rotation bitmask (rotate-0=0x1,
    # rotate-90=0x2, rotate-180=0x4, rotate-270=0x8).
    rot_value=2

    # Find plane object ids from `modetest -p` and try each until one accepts a
    # 'rotation' property. modetest is a test tool that may wait for input, so
    # feed it EOF and bound it with a timeout.
    planes=$(modetest -p 2>/dev/null | awk '
        /^Planes:/      { inplanes = 1; next }
        /^[A-Za-z].*:/  { if (inplanes) inplanes = 0 }
        inplanes && $1 ~ /^[0-9]+$/ { print $1 }')

    if [ -z "$planes" ]; then
        log "no DRM planes found via modetest -- cannot rotate"
        return 1
    fi

    for pid in $planes; do
        log "attempting ${ROTATE_DEG}deg rotation on plane $pid"
        if echo | timeout 3 modetest -w "${pid}:rotation:${rot_value}" >/dev/null 2>&1; then
            log "rotation applied on plane $pid (${ROTATE_DEG}deg)"
            return 0
        fi
    done

    log "no plane accepted a rotation property -- driver likely lacks HW rotation"
    return 1
}

# ---- Main -------------------------------------------------------------------
conn=$(detect_connector) || {
    log "no connected DRM panel found -- skipping"
    exit 0
}

case "$conn" in
    *LVDS*|*lvds*)
        echo "LVDS detected"
        ;;
    *DSI*|*dsi*|*MIPI*|*mipi*)
        echo "MIPI detected"
        rotate_90 || true
        ;;
    *)
        log "connected panel '$conn' is neither LVDS nor MIPI/DSI -- no action"
        ;;
esac

exit 0
