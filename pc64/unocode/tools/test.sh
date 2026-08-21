# VENDORED FILE - DO NOT EDIT HERE.
#
# UnoCode is developed at https://github.com/hmofet/unocode-desktop, in its core/ directory.
# An edit made here is lost at the next sync, and until then it silently
# forks the editor away from the tree the desktop builds are cut from.
#
# Change it there; bring it back with pc64/tools/sync_unocode.py.
# See pc64/UNOCODE-UPSTREAM.md.
#!/bin/sh
# UnoCode core tests - the JSONC parser and the regex engine, built natively.
#
# These two files are pure logic (no framebuffer, no toolkit, no filesystem),
# so they can be tested on the build host in a second instead of through a
# QEMU boot.  Every theme, keybinding, snippet, manifest and grammar in the
# product is read through them.
#
#   sh core/tools/test.sh          (UnoCode Desktop, where the core lives)
#   sh unocode/tools/test.sh       (UnoDOS, where it is vendored)
#
# THE SAME FILE RUNS IN BOTH TREES, on purpose.  It is vendored verbatim along
# with the core it tests, so it finds its own layout rather than carrying two
# copies that can drift:
#
#   UnoDOS         <root>/pc64/unocode/  + <root>/unoui, <root>/ps2
#   Desktop        <root>/core/          + <root>/upstream/unodos/{unoui,ps2}
#
# ./build.sh --gate runs this as its first stage in the desktop tree, and
# pc64/tools/gate.sh runs it in UnoDOS.
set -e
CORE=$(cd "$(dirname "$0")/.." && pwd)

if [ -d "$CORE/../../unoui" ]; then            # vendored into UnoDOS
    U=$(cd "$CORE/../.." && pwd)
elif [ -d "$CORE/../upstream/unodos/unoui" ]; then   # UnoCode Desktop
    U=$(cd "$CORE/../upstream/unodos" && pwd)
else
    echo "test.sh: cannot find unoui from $CORE - is the submodule checked out?" >&2
    exit 1
fi

CC="${CC:-gcc}"
mkdir -p "$CORE/build"
$CC -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -I"$CORE" -I"$U/pc64" -I"$U/unoui" -I"$U/ps2" \
    "$CORE/uc_json.c" "$CORE/uc_rx.c" "$CORE/uc_util.c" "$CORE/tools/uc_test.c" \
    -o "$CORE/build/uc_test"
"$CORE/build/uc_test"
