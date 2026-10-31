#!/bin/sh
# Build notepad-route, install the binary, and (re)load the launchd agent.
#   ./install.sh            install or upgrade
#   ./install.sh uninstall  stop the agent and remove everything
set -eu
cd "$(dirname "$0")"

LABEL=local.notepad-route
BIN=/usr/local/bin/notepad-route
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/notepad-route.log"
DOMAIN="gui/$(id -u)"

if [ "${1:-}" = "uninstall" ]; then
    launchctl bootout "$DOMAIN" "$PLIST" 2>/dev/null || true
    rm -f "$PLIST"
    sudo rm -f "$BIN"
    echo "removed agent and $BIN (log and remembered routing left in place)"
    exit 0
fi

echo "building..."
clang -O2 -Wall -o notepad-route notepad_route.c -framework IOKit -framework CoreFoundation

echo "installing $BIN (sudo)..."
sudo mkdir -p /usr/local/bin
sudo install -m 755 notepad-route "$BIN"

mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
sed "s|@HOME@|$HOME|g" "$LABEL.plist" > "$PLIST"

# Unload any previous copy, then load this one. bootout fails harmlessly if nothing was loaded.
launchctl bootout "$DOMAIN" "$PLIST" 2>/dev/null || true
launchctl bootstrap "$DOMAIN" "$PLIST"

echo
echo "installed. the agent is running now and will start at every login."
echo "  set routing:   notepad-route 0        (0=MIC 1+2  1=LINE 3/4  2=LINE 5/6  3=MASTER)"
echo "  check:         notepad-route --status"
echo "  agent log:     tail -f $LOG"
echo "  agent state:   launchctl print $DOMAIN/$LABEL | head -20"
