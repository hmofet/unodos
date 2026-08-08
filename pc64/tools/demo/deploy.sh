#!/bin/sh
# deploy - put the demo driver on the METAL driver host.
#
# The X13 Yoga's stick dials OUT to a host named in its DEBUG.CFG
# (remote=<host>:5101), so the driver has to RUN on that host - a Windows dev
# box cannot accept the inbound connection. devbuntu is that host.
#
#   sh tools/demo/deploy.sh [user@host] [destdir]
#
# Then, ON the host:
#   pkill -f '[w]atcher.py'            # frees :5101 if a watcher holds it
#   cd ~/demo && python3 scenes.py --metal --all
#
# scp, not rsync: this is run from the Windows dev box under Git Bash, which
# ships OpenSSH but no rsync.
set -e
HOST="${1:-devbuntu}"
DEST="${2:-demo}"
HERE=$(cd "$(dirname "$0")" && pwd)
PC64=$(dirname "$(dirname "$HERE")")
REPO=$(dirname "$PC64")

ssh "$HOST" "mkdir -p $DEST/corpus"
# the driver + everything it imports. remote_qemu.py comes along so the module
# imports cleanly, but --metal never calls it (and tolerates its absence).
scp -q "$HERE/scenes.py" "$HERE/stream_recv.py" "$HERE/SCENES.md" \
       "$PC64/tools/unoauto_remote.py" "$PC64/tools/urcui.py" \
       "$PC64/tools/remote_qemu.py" \
       "$HOST:$DEST/"
# the four office documents s04 pushes to the RAM volume (the repo is not on
# the driver host, so scenes.py falls back to ./corpus)
scp -q "$REPO/unodoc/test/corpus/fmt.doc" \
       "$REPO/unodoc/test/corpus/pic.doc" \
       "$REPO/unodoc/test/corpus/formulas.xls" \
       "$REPO/unodoc/test/corpus/small.ppt" \
       "$HOST:$DEST/corpus/"

echo "deployed to $HOST:$DEST - now, on $HOST:"
echo "    pkill -f '[w]atcher.py'"
echo "    cd $DEST && python3 scenes.py --metal --all"
