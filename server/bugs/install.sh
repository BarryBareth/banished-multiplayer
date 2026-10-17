#!/bin/sh
# Installs the bug report receiver as a systemd service on 127.0.0.1:8781. Run with sudo from this folder
# (server/bugs of the repository). It listens on localhost only: put a reverse proxy with HTTPS in front of it
# (nginx, Caddy, a Cloudflare tunnel...) - see docs/BUG_REPORTS.md.
set -e
cd "$(dirname "$0")"
install -d /opt/bmp-bugs
install -m 644 ../../tools/bmp_bugs.py /opt/bmp-bugs/bmp_bugs.py
install -m 644 bmp-bugs.service /etc/systemd/system/bmp-bugs.service
systemctl daemon-reload
systemctl enable --now bmp-bugs
systemctl restart bmp-bugs
sleep 1
curl -s http://127.0.0.1:8781/health && echo
systemctl --no-pager --lines=3 status bmp-bugs || true
echo "Reports land in /var/lib/bmp-bugs/reports. Logs: journalctl -u bmp-bugs -f"
