#!/bin/sh
# Keep hyprcross.so in step with the running Hyprland. Run at login (cross.lua does).
# 1. hyprpm update refreshes the headers in /var/cache/hyprpm when Hyprland changed.
# 2. make rebuilds only if those headers are newer than the .so.
# 3. If the plugin couldn't load at startup (old build), load the fresh one now.
cd "$(dirname "$0")/.." || exit 1
log=${XDG_STATE_HOME:-$HOME/.local/state}/hyprcross-update.log
mkdir -p "$(dirname "$log")"
{
    date
    hyprpm update --no-nix
    make install
    hyprctl plugin list | grep -q hyprcross || hyprctl plugin load "$HOME/.local/lib/hyprland/hyprcross.so"
} >>"$log" 2>&1
