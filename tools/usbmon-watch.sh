#!/usr/bin/env bash
#
# Traces the fingerprint sensor's bulk endpoints at the kernel URB level.
#
# Usage: sudo tools/usbmon-watch.sh
#
# Unlike tools/trace-watch.sh, which renders our own libusb calls, this sees
# the traffic whoever generates it -- so it is the way to watch what
# libfprint/fprintd actually put on the wire, and the way to tell a device
# that NAKs forever apart from one that stalls or is never asked anything.
# Those are three different bugs that look identical from libusb's API.
#
# Requires the usbmon module: modprobe usbmon

set -uo pipefail

VID=045e
PID=09c2

sysdir=""
for d in /sys/bus/usb/devices/*/; do
    [ -f "$d/idVendor" ] || continue
    [ "$(cat "$d/idVendor")" = "$VID" ] || continue
    [ "$(cat "$d/idProduct")" = "$PID" ] || continue
    sysdir="$d"; break
done

if [ -z "$sysdir" ]; then
    echo "device $VID:$PID not found (is the Type Cover attached?)" >&2
    exit 1
fi

bus=$(cat "$sysdir/busnum")
dev=$(cat "$sysdir/devnum")
node="/sys/kernel/debug/usb/usbmon/${bus}u"

if [ ! -r "$node" ]; then
    echo "cannot read $node -- run as root, and modprobe usbmon" >&2
    exit 1
fi

printf 'watching %s:%s on bus %s device %s (endpoints 4 out, 3 in)\n\n' \
       "$VID" "$PID" "$bus" "$dev"

# usbmon '1u' text: tag timestamp_us event address status length '=' data
# address looks like Bo:3:030:4 -- bulk out, bus 3, device 30, endpoint 4.
# Only the sensor's own bulk endpoints matter; interface 0 is the keyboard
# and its interrupt traffic would bury everything.
awk -v bus="$bus" -v dev="$dev" '
  BEGIN {
    devpat = sprintf(":%d:%03d:", bus, dev)
    t0 = 0
  }
  index($4, devpat) == 0 { next }
  {
    split($4, a, ":")
    ep = a[4]
    if (ep != "4" && ep != "3") next

    if (t0 == 0) t0 = $2
    ms = ($2 - t0) / 1000.0

    ev = $3                      # S submit, C complete, E error
    status = $5
    len = $6
    data = ""
    for (i = 8; i <= NF; i++) data = data $i " "

    dir = (ep == "4") ? "OUT" : "IN "
    label = (ev == "S") ? "submit  " : (ev == "C") ? "complete" : "error   "

    printf "%10.3f  %s %s  len=%-6s status=%-5s %s\n", ms, dir, label, len, status, data
    fflush()
  }' "$node"
