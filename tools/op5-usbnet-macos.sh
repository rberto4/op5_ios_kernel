#!/bin/bash
# macOS host side of the phone's USB Ethernet (kernel sweetusb=2, patch 0096).
#   tools/op5-usbnet-macos.sh [LOGFILE]
# Waits for the phone's CDC-ECM interface (host-side MAC 02:00:5e:77:ee:01),
# gives it 192.168.7.1/24 (the phone is 192.168.7.2), then prints the live
# kernel console the phone streams to UDP port 5140 (and appends it to
# LOGFILE if given). Needs sudo for the address (asked once).
set -euo pipefail
mac=02:00:5e:77:ee:01
find_if() {
	ifconfig -a | awk -v mac="$mac" '
		/^[a-z0-9]+: / { name = substr($1, 1, length($1) - 1) }
		tolower($0) ~ "ether " mac { print name; exit }'
}
echo "waiting for the phone's USB Ethernet ($mac)..." >&2
ifname=$(find_if)
until [ -n "$ifname" ]; do
	sleep 1
	ifname=$(find_if)
done
sudo ifconfig "$ifname" inet 192.168.7.1 netmask 255.255.255.0 up
echo "$ifname up: 192.168.7.1 <-> phone 192.168.7.2; live log on udp/5140" >&2
exec python3 -I -u -c '
import socket, sys
log = open(sys.argv[1], "ab") if len(sys.argv) > 1 else None
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("192.168.7.1", 5140))
while True:
    data, _ = s.recvfrom(65536)
    sys.stdout.buffer.write(data)
    sys.stdout.flush()
    if log:
        log.write(data)
        log.flush()
' "$@"
