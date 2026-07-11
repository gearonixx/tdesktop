#!/bin/bash
# PID 1 for the tdesktop dev container. Brings sshd up so CLion / Gateway /
# VSCode Remote-SSH can connect, then keeps the container alive.
set -e

# Host keys + runtime dir (need root). The Rocky base image ships a leftover
# /run/nologin that makes pam_nologin reject every login ("System is booting
# up"), so clear it before sshd comes up.
sudo ssh-keygen -A
sudo mkdir -p /run/sshd
sudo rm -f /run/nologin

# Per-container key pair the host copies out to authenticate.
mkdir -p "$HOME/.ssh"
chmod 700 "$HOME/.ssh"
if [ ! -f "$HOME/.ssh/clion_key" ]; then
    ssh-keygen -t ed25519 -f "$HOME/.ssh/clion_key" -N "" -C "clion@tdesktop" >/dev/null
fi
touch "$HOME/.ssh/authorized_keys"
chmod 600 "$HOME/.ssh/authorized_keys"
if ! grep -qxF "$(cat "$HOME/.ssh/clion_key.pub")" "$HOME/.ssh/authorized_keys"; then
    cat "$HOME/.ssh/clion_key.pub" >> "$HOME/.ssh/authorized_keys"
fi

if ! pgrep -fx "/usr/sbin/sshd -f /etc/ssh/sshd_config" >/dev/null; then
    sudo /usr/sbin/sshd -f /etc/ssh/sshd_config || echo "WARNING: sshd failed to start" >&2
fi

if [ "$#" -eq 0 ]; then
    exec sleep infinity
fi

exec "$@"
