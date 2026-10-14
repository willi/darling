#!/bin/sh
# Collect the os_log messages of an iOS Simulator device (inside `darling shell`) into a directory laid out as
# `log collect` lays out a .logarchive: the device's logd store (tracev3 files and their timesync) together with the
# format strings it saved for each binary (uuidtext). The runtime has no `log` tool to read it with; a .logarchive
# reader such as mandiant/macos-UnifiedLogs reads it on Linux.
#
# usage: simlogarchive.sh <directory>
#
# logd writes its store when the device shuts down, so this runs on a device that simshutdown.sh shut down.
set -eu
# shellcheck source=simdevice.sh
. "$(dirname "$0")/simdevice.sh"

if [ $# -ne 1 ]; then
	echo "usage: $0 <directory>" >&2
	exit 64
fi
OUT="$1"
STORE="$SIM_DEVICE_DATA/var/db/diagnostics"
STRINGS="$SIM_DEVICE_DATA/var/db/uuidtext"

if launchctl list "$SIM_DEVICE_JOB_LABEL" >/dev/null 2>&1; then
	echo "$SIM_DEVICE_JOB_LABEL is booted, and logd only writes its store at shutdown: run simshutdown.sh first" >&2
	exit 1
fi
if [ ! -d "$STORE/Persist" ]; then
	echo "$STORE has no logs: is logd in the device's SIM_DAEMONS?" >&2
	exit 1
fi
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT")" ]; then
	echo "$OUT isn't empty" >&2
	exit 1
fi

created=false
[ -e "$OUT" ] || created=true
mkdir -p "$OUT"
if ! cp -R "$STORE/." "$OUT/" || { [ -d "$STRINGS" ] && ! cp -R "$STRINGS/." "$OUT/"; }; then
	# leave nothing half-copied behind (it would be refused as non-empty next time)
	if $created; then
		rm -rf "$OUT"
	else
		find "$OUT" -mindepth 1 -delete
	fi
	echo "couldn't copy the logs into $OUT" >&2
	exit 1
fi
echo "$OUT"
