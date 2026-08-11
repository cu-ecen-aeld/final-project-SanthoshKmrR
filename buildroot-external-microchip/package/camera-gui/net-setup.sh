#!/bin/sh
# Point-to-point network setup for the SAM9X75 (camera source / sender).
# Direct cable to the SAMA7D65 & SAMA7G5. No DHCP, no gateway.
#
# SAM9X75  = 192.168.10.10
# SAMA7D65 = 192.168.10.22

set -e

IFACE="${1:-eth0}"          # Gigabit MAC interface on the SAMA7D65
ADDR="192.168.10.22/24"

ip link set "$IFACE" up
ip addr flush dev "$IFACE"
ip addr add "$ADDR" dev "$IFACE"

# --- Optional: jumbo frames (both ends must match). Big win for raw RTP,
#     which is packet-heavy. Only enable if both PHYs/MACs support it.
# ip link set "$IFACE" mtu 5000

echo "sender $IFACE -> $ADDR"
ip addr show dev "$IFACE" | sed -n 's/^ *inet /  inet /p'
