#!/bin/sh

# Minimal tested custom.sh for the stock SD hook.
# This version keeps only the pieces needed for:
# - telnet on port 24
# - the vendor RTSP bootstrap loop

if [ ! -e /tmp/custom ]; then
 touch /tmp/custom
 telnetd -p 24 -l /bin/sh
fi

if [ -x /tmp/sd/vendor_rtsp_boot.sh ]; then
 /tmp/sd/vendor_rtsp_boot.sh
fi
