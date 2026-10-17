#!/bin/sh
# Installs the Banished Multiplayer relay as a systemd service on TCP port 7780. Run with sudo from this folder (server/relay of the repository).
set -e
cd "$(dirname "$0")"
install -d /opt/bmp-relay
install -m 644 ../../tools/bmp_relay.py /opt/bmp-relay/bmp_relay.py
install -m 644 bmp-relay.service /etc/systemd/system/bmp-relay.service
systemctl daemon-reload
systemctl enable --now bmp-relay
systemctl restart bmp-relay
if command -v ufw >/dev/null 2>&1 && ufw status | grep -q "Status: active"; then
    ufw allow 7780/tcp comment 'Banished Multiplayer relay'
fi
sleep 1
systemctl --no-pager --lines=3 status bmp-relay || true
echo
echo "LAN address of this server (forward TCP 7780 on the router to it):"
ip -4 -o addr show scope global | awk '{print "  " $4}'
echo "Public address: $(curl -s --max-time 5 https://api.ipify.org || echo unknown)"
echo "Logs: journalctl -u bmp-relay -f"
