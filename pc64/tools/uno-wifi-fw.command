#!/bin/sh
# macOS double-click launcher for uno-wifi-fw.py.
# (If double-click is blocked, run:  chmod +x uno-wifi-fw.command  once.)
cd "$(dirname "$0")"
python3 uno-wifi-fw.py "$@"
echo ""
printf "Press Return to close..."; read _
