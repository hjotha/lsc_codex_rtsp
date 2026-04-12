killall anyka_ring_rtsp_server >/dev/null 2>&1 || true
/tmp/sd/anyka_ring_rtsp_server --ring /tmp/VideoMainStream0 --port 8554 --loop-forever --verbose >/tmp/sd/reader8554.log 2>&1 &
sleep 1
netstat -lnpt 2>/dev/null || netstat -lnp 2>/dev/null
