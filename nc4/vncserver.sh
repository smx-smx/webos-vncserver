#!/bin/sh
# webos-vncserver on LG NetCast 4.x, as root.
#
#   vncserver.sh [start|stop|status|run]
#
# Settings (VNC_PASSWORD, VNC_WIDTH, VNC_HEIGHT, VNC_FRAMERATE) can be put in
# vncserver.env next to this script. start logs to vncserver.log.
DIR=$(cd "$(dirname "$0")" && pwd -P)
[ -f "$DIR/vncserver.env" ] && . "$DIR/vncserver.env"
export VNC_PASSWORD VNC_WIDTH VNC_HEIGHT VNC_FRAMERATE

case "${1:-start}" in
run)
    exec "$DIR/webos-vncserver"
    ;;
start)
    if pidof webos-vncserver > /dev/null; then
        echo "already running"
        exit 0
    fi
    nohup "$DIR/webos-vncserver" >> "$DIR/vncserver.log" 2>&1 < /dev/null &
    sleep 1
    pidof webos-vncserver > /dev/null && echo "started, port 5900" || { echo "failed, see $DIR/vncserver.log"; exit 1; }
    ;;
stop)
    pid=$(pidof webos-vncserver) || { echo "not running"; exit 0; }
    kill $pid
    echo "stopped"
    ;;
status)
    pidof webos-vncserver > /dev/null && echo "running" || echo "not running"
    ;;
*)
    echo "usage: $0 [start|stop|status|run]"
    exit 1
    ;;
esac
