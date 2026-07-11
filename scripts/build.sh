#!/usr/bin/env bash
# Cross-compile Smack for the Ableton Move (aarch64 Linux) via Docker,
# mirroring schwung's own toolchain (debian:bookworm + gcc-aarch64-linux-gnu)
# so the .so links against the same glibc as the rest of the ecosystem.
#
# Outputs:
#   build/modules/audio_fx/smack/smack.so + module.json   (chain + master FX)
#   build/modules/sound_generators/smack-in/dsp.so + module.json
#   build/smack-module.tar.gz, build/smack-in-module.tar.gz
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=smack-build
CFLAGS="-O3 -g -shared -fPIC -Wall -Wextra -Iinclude"

if ! docker image inspect "$IMAGE" &>/dev/null; then
    docker build -t "$IMAGE" - <<'EOF'
FROM debian:bookworm
RUN apt-get update && apt-get install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu && rm -rf /var/lib/apt/lists/*
EOF
fi

mkdir -p build/modules/audio_fx/smack build/modules/sound_generators/smack-in

docker run --rm -v "$PWD":/w -w /w "$IMAGE" bash -c "
    set -e
    aarch64-linux-gnu-gcc $CFLAGS src/smack_core.c src/smack_fx.c \
        -o build/modules/audio_fx/smack/smack.so -lm
    aarch64-linux-gnu-gcc $CFLAGS src/smack_core.c src/smack_gen.c \
        -o build/modules/sound_generators/smack-in/dsp.so -lm
    file build/modules/audio_fx/smack/smack.so build/modules/sound_generators/smack-in/dsp.so
"

cp modules/audio_fx/smack/module.json build/modules/audio_fx/smack/
cp modules/sound_generators/smack-in/module.json build/modules/sound_generators/smack-in/

tar -czf build/smack-module.tar.gz -C build/modules/audio_fx smack
tar -czf build/smack-in-module.tar.gz -C build/modules/sound_generators smack-in
echo "Built: build/smack-module.tar.gz, build/smack-in-module.tar.gz"
