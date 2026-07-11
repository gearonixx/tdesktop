# Telegram Desktop dev container.
#
# Layered on top of the official build image (tdesktop:centos_env), which
# already ships the full toolchain (gcc-toolset, cmake, ninja, Qt, ffmpeg,
# webrtc and every prepared third-party lib). Here we only add the bits needed
# to use it as a remote IDE host: sshd on port 2222, passwordless sudo for the
# build `user`, and the Claude CLI.
#
# Build the base image first if you don't have it:
#     docker build -t tdesktop:centos_env Telegram/build/docker/centos_env
#
# Then build this one (see dev/README.md or just run ./dev/link):
#     docker build -t tdesktop-dev dev/

FROM tdesktop:centos_env

USER root

# SSH server + everyday dev conveniences. The toolchain itself already lives in
# the base image, so this stays small.
RUN dnf -y install \
        openssh-server sudo procps-ng \
        git-lfs rsync tmux vim nano less tree jq htop \
    && dnf clean all

# Passwordless sudo for the build user, plus a password fallback (user/user)
# for IDEs that insist on prompting.
RUN echo "user ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/user \
    && chmod 0440 /etc/sudoers.d/user \
    && echo "user:user" | chpasswd

# sshd on a non-default port so it never clashes with the host's own sshd.
# internal-sftp avoids hardcoding the sftp-server binary path across distros.
RUN printf '%s\n' \
        'Port 2222' \
        'PermitRootLogin no' \
        'PubkeyAuthentication yes' \
        'PasswordAuthentication yes' \
        'ChallengeResponseAuthentication no' \
        'UsePAM yes' \
        'X11Forwarding no' \
        'PrintMotd no' \
        'AcceptEnv LANG LC_*' \
        'Subsystem sftp internal-sftp' \
        > /etc/ssh/sshd_config \
    && mkdir -p /run/sshd \
    && rm -f /etc/ssh/ssh_host_*
EXPOSE 2222

# SSH sessions (including the non-interactive ones CLion uses to detect the
# toolchain) don't inherit Docker ENV, so they'd see the system gcc 8.5 instead
# of the gcc-toolset the build actually uses. Persist the base image's toolchain
# environment into /etc/environment, which PAM applies to every SSH login.
# Values are taken from the inherited ENV so the toolset version is never
# hardcoded. /home/user/.local/bin is prepended for the Claude CLI.
RUN printf '%s\n' \
        "PATH=/home/user/.local/bin:${PATH}" \
        "TOOLSET=${TOOLSET}" \
        "LIBRARY_PATH=${LIBRARY_PATH}" \
        "LD_LIBRARY_PATH=${LD_LIBRARY_PATH}" \
        "PKG_CONFIG_PATH=${PKG_CONFIG_PATH}" \
        > /etc/environment

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod 0755 /usr/local/bin/entrypoint.sh

USER user
WORKDIR /usr/src/tdesktop

# Claude CLI, same convenience aliases as the ytsaurus dev container.
RUN curl -fsSL https://claude.ai/install.sh | bash \
 && printf '%s\n' \
        'export PATH="$HOME/.local/bin:$PATH"' \
        "alias claude='claude --dangerously-skip-permissions --permission-mode bypassPermissions'" \
        >> ~/.bashrc
ENV PATH=/home/user/.local/bin:${PATH}

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["sleep", "infinity"]
