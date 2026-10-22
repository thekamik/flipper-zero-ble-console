#!/bin/sh
# Installs piterm-server from the directory containing this script.
# Usage: sudo ./install.sh [user]
set -eu
cd "$(dirname "$0")"
USER_NAME="${1:-${SUDO_USER:-pi}}"

install -m 755 piterm-server /usr/local/bin/piterm-server
mkdir -p /etc/piterm
[ -f /etc/piterm/config.toml ] || install -m 644 config.toml /etc/piterm/config.toml
sed "s/^User=.*/User=$USER_NAME/" piterm.service > /etc/systemd/system/piterm.service
usermod -aG bluetooth "$USER_NAME" 2>/dev/null || true

systemctl daemon-reload
echo "Installed. Next:"
echo "  1. start PiTerm on the Flipper"
echo "  2. pair with \"PiTerm <name>\" (bluetoothctl, or: sudo piterm-server pair <MAC> --save)"
echo "  3. sudo systemctl enable --now piterm"
