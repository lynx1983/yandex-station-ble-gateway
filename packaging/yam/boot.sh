#!/bin/sh
# Managed by the lynx/redmond-kettle YAM package.
service=/data/yam/bin/redmond-kettle
[ -x "$service" ] || exit 0
case "${1:-start}" in
    start|stop|restart|status) exec "$service" "${1:-start}" ;;
    *) exit 2 ;;
esac
