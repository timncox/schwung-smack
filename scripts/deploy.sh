#!/usr/bin/env bash
# Copy built modules onto the Move. Requires ssh access (ableton@move.local,
# same as schwung's installer). Run scripts/build.sh first.
#
# After deploying, rescan modules from the schwung UI (or restart Move) so
# the host picks them up: host_rescan_modules() runs on rescan.
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${MOVE_HOST:-ableton@move.local}"
DEST=/data/UserData/schwung/modules

[ -f build/modules/audio_fx/smack/smack.so ] || { echo "run scripts/build.sh first"; exit 1; }

ssh "$HOST" "mkdir -p $DEST/audio_fx $DEST/sound_generators $DEST/overtake"
scp -r build/modules/audio_fx/smack "$HOST:$DEST/audio_fx/"
scp -r build/modules/sound_generators/smack-in "$HOST:$DEST/sound_generators/"
scp -r build/modules/overtake/oversmack "$HOST:$DEST/overtake/"
echo "Deployed to $HOST:$DEST — rescan modules or restart Move."
