# Telegram Desktop dev container

Remote IDE container for Telegram Desktop, mirroring the ytsaurus `dev/` setup.

It layers an SSH server + Claude CLI on top of the official build image
(`tdesktop:centos_env`, Rocky Linux 8 with the full gcc-toolset / cmake / ninja
/ Qt / ffmpeg / webrtc toolchain), so CLion / JetBrains Gateway / VSCode
Remote-SSH can build and index the project inside the container.

Non-root `user` (uid 1000) with passwordless sudo, sshd on port 2222.

## One command

```bash
./dev/link
```

`link` is idempotent. On first run it builds the image and creates the
container (mounting the repo at `/usr/src/tdesktop`); on every run it makes sure
the container is up, starts sshd, copies the SSH key to
`~/.ssh/tdesktop_clion_key`, and verifies the host can connect. It prints the
exact Host / Port / User / Key for CLion at the end.

> First run is slow only if the base image `tdesktop:centos_env` is missing —
> `link` builds it from `Telegram/build/docker/centos_env` (large). If you've
> already built tdesktop in Docker once, it's cached and reused.

## Manual equivalents

```bash
# Base toolchain image (only if missing — large, slow):
docker build -t tdesktop:centos_env Telegram/build/docker/centos_env

# This ssh/dev layer:
docker build -t tdesktop-clion dev/

# Run it (source mounted, ccache volume):
docker run -d \
    --name tdesktop-clion \
    -p 2222:2222 \
    -v "$PWD":/usr/src/tdesktop \
    -v tdesktop-ccache:/home/user/.cache \
    -w /usr/src/tdesktop \
    tdesktop-clion

docker start tdesktop-clion
docker stop  tdesktop-clion
docker exec -it tdesktop-clion bash
```

## SSH / Remote IDE

After `./dev/link`, connect with the key it dropped:

```bash
ssh -i ~/.ssh/tdesktop_clion_key -p 2222 user@<container-ip>
```

Find the IP with:

```bash
docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' tdesktop-clion
```

Password fallback: `user` / `user`.

**CLion / Gateway:** Remote Host `localhost:2222` (or the container IP), user
`user`, key `~/.ssh/tdesktop_clion_key`. Point the CMake project at
`/usr/src/tdesktop/Telegram/CMakeLists.txt`.

**VSCode** Remote-SSH:

```
Host tdesktop-clion
    HostName localhost
    Port 2222
    User user
    IdentityFile ~/.ssh/tdesktop_clion_key
```

## Building Telegram inside the container

Use your own `api_id` / `api_hash` (see `docs/api_credentials.md`):

```bash
./Telegram/build/docker/centos_env/build.sh \
    -D TDESKTOP_API_ID=YOUR_API_ID \
    -D TDESKTOP_API_HASH=YOUR_API_HASH
```

Built output lands in `out/`.

## sshd not up?

If the container got started without `entrypoint.sh` (e.g. a bare
`docker exec`), bring sshd up by hand:

```bash
bash /usr/src/tdesktop/dev/manually-start-ssh.sh
```

`./dev/link` does this for you on every run.
