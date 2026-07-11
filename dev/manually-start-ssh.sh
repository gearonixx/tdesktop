#!/bin/bash
# Manually start sshd inside the dev container so CLion (or any external SSH
# client) can connect.
#
# You normally don't need this — entrypoint.sh starts sshd automatically. Run
# this only when sshd isn't up, typically because the container was started with
# bash as PID 1 (e.g. via `docker exec` or an `--entrypoint` override) instead
# of through entrypoint.sh. The host-side ./dev/link calls it for you.
#
# Usage (from inside the container):
#     bash /usr/src/tdesktop/dev/manually-start-ssh.sh
#
# Connect from outside:
#     ssh -i ~/.ssh/clion_key -p 2222 user@<container-ip>

set -e

echo "[1/4] Generating sshd host keys (if missing) and runtime dir..."
sudo ssh-keygen -A
sudo mkdir -p /run/sshd
sudo rm -f /run/nologin   # Rocky image ships one; pam_nologin would reject logins

echo "[2/4] Ensuring ~/.ssh and clion key are set up..."
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

echo "[3/4] Starting sshd on port 2222..."
if pgrep -fx "/usr/sbin/sshd -f /etc/ssh/sshd_config" >/dev/null; then
    echo "      sshd already running"
else
    sudo /usr/sbin/sshd -f /etc/ssh/sshd_config
fi

echo "[4/4] Verifying..."
IP="$(hostname -I | awk '{print $1}')"
if ssh -i "$HOME/.ssh/clion_key" \
       -o StrictHostKeyChecking=no \
       -o UserKnownHostsFile=/dev/null \
       -o LogLevel=ERROR \
       -p 2222 "user@${IP}" 'echo SSH_OK' 2>/dev/null | grep -q SSH_OK; then
    echo
    echo "OK — SSH is up. Connect from your host with:"
    echo "    ssh -i ~/.ssh/clion_key -p 2222 user@${IP}"
    echo
    echo "CLion: Host=${IP}  Port=2222  User=user  Key=/home/user/.ssh/clion_key"
else
    echo "WARNING: sshd started but self-test failed. Check 'ps -ef | grep sshd'." >&2
    exit 1
fi
