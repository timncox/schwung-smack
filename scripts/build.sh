#!/usr/bin/env bash
# Cross-compile Smack for the Ableton Move (aarch64 Linux) via Docker,
# mirroring schwung's own toolchain (debian:bookworm + gcc-aarch64-linux-gnu)
# so the .so links against the same glibc as the rest of the ecosystem.
#
# Outputs:
#   build/modules/audio_fx/smack/smack.so + module.json   (chain + master FX)
#   build/modules/sound_generators/smack-in/dsp.so + module.json
#   build/modules/overtake/oversmack/dsp.so + module.json (full-surface UI)
#   build/smack-module.tar.gz, build/smack-in-module.tar.gz,
#   build/oversmack-module.tar.gz
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=smack-build
CFLAGS="-O3 -g -shared -fPIC -Wall -Wextra -Iinclude"

if ! docker image inspect "$IMAGE" &>/dev/null; then
    docker build -t "$IMAGE" - <<'EOF'
FROM debian:bookworm
RUN apt-get update && apt-get install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu file && rm -rf /var/lib/apt/lists/*
EOF
fi

mkdir -p build/modules/audio_fx/smack build/modules/sound_generators/smack-in \
         build/modules/overtake/oversmack

cp modules/audio_fx/smack/module.json src/ui_chain.js build/modules/audio_fx/smack/
cp src/help.json build/modules/audio_fx/smack/help.json
cp modules/sound_generators/smack-in/module.json src/ui_chain.js build/modules/sound_generators/smack-in/
cp src/help_smack_in.json build/modules/sound_generators/smack-in/help.json
cp modules/overtake/oversmack/module.json build/modules/overtake/oversmack/
cp src/ui_overtake.js build/modules/overtake/oversmack/ui.js
cp src/help_oversmack.json build/modules/overtake/oversmack/help.json

# Compile AND tar inside the container: macOS bsdtar embeds AppleDouble
# (._*) xattr entries that Linux tar extracts as real files — the schwung
# installer then reads entries[0] = "._smack" and fails with "No module.json
# found in tarball". GNU tar in the container produces clean archives.
docker run --rm -v "$PWD":/w -w /w "$IMAGE" bash -c "
    set -e
    aarch64-linux-gnu-gcc $CFLAGS src/smack_core.c src/smack_fx.c \
        -o build/modules/audio_fx/smack/smack.so -lm
    aarch64-linux-gnu-gcc $CFLAGS src/smack_core.c src/smack_gen.c \
        -o build/modules/sound_generators/smack-in/dsp.so -lm
    cp build/modules/sound_generators/smack-in/dsp.so build/modules/overtake/oversmack/dsp.so
    file build/modules/audio_fx/smack/smack.so build/modules/sound_generators/smack-in/dsp.so
    tar --owner=0 --group=0 -czf build/smack-module.tar.gz -C build/modules/audio_fx smack
    tar --owner=0 --group=0 -czf build/smack-in-module.tar.gz -C build/modules/sound_generators smack-in
    tar --owner=0 --group=0 -czf build/oversmack-module.tar.gz -C build/modules/overtake oversmack
    echo 'tarball contents:'
    tar -tzf build/smack-module.tar.gz
    tar -tzf build/oversmack-module.tar.gz
"

echo "Built: build/smack-module.tar.gz, build/smack-in-module.tar.gz, build/oversmack-module.tar.gz"
