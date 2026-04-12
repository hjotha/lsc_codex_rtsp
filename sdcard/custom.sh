#!/bin/sh

# Minimal startup hook for the SD boot path.
# We only need telnet for diagnostics and vendor_rtsp_boot.sh for RTSP.

if [ ! -e /tmp/custom ]; then
 touch /tmp/custom
 telnetd -p 24 -l /bin/sh
fi

if [ -x /tmp/sd/vendor_rtsp_boot.sh ]; then
 /tmp/sd/vendor_rtsp_boot.sh
fi
