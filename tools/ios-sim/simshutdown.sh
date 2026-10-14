#!/bin/sh
# Shut down an iOS Simulator device that simboot.sh booted (inside `darling shell`), as `simctl shutdown` does: its
# job in Darling's launchd is unloaded, launchd_sim gets SIGTERM and stops the device's processes, and the device's
# data stays for the next boot.
#
# usage: simshutdown.sh
set -eu
# shellcheck source=simdevice.sh
. "$(dirname "$0")/simdevice.sh"

if [ $# -gt 0 ]; then
	echo "usage: $0" >&2
	exit 64
fi
if ! launchctl list "$SIM_DEVICE_JOB_LABEL" >/dev/null 2>&1; then
	echo "$SIM_DEVICE_JOB_LABEL is not booted" >&2
	exit 0
fi

# returns once launchd_sim has exited
launchctl unload "$SIM_DEVICE_JOB"
echo "shut down $SIM_DEVICE_JOB_LABEL"
