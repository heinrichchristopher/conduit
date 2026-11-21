#!/bin/bash
# Run Conduit in xemu with a NAT setup that forwards FTP to the host:
#   host 127.0.0.1:2121 -> Xbox :21,  host 50000-50099 -> Xbox 50000-50099 (passive mode)
#
# Needs your own xemu config (BIOS, MCPX, flash, HDD image) that includes tools/xemu-nat.toml.example
# ([net] section). Controller 1 can be set to 'keyboard' there (B key = B, Backspace = BACK).
#
#   XEMU_CONFIG=~/xemu-conduit.toml tools/xemu_ftp.sh [path/to/Conduit.iso]
#
# Note: xemu rewrites its config on a normal exit; keep a copy.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="${1:-$ROOT/Conduit.iso}"
CFG="${XEMU_CONFIG:?set XEMU_CONFIG to your xemu config file}"
if command -v xemu >/dev/null; then
  exec xemu -config_path "$CFG" -dvd_path "$ISO"
else
  exec flatpak run --filesystem="$(dirname "$CFG")" --filesystem="$(dirname "$ISO")" app.xemu.xemu \
    -config_path "$CFG" -dvd_path "$ISO"
fi
