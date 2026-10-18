#!/bin/sh
# Shut down an iOS Simulator device that simboot.sh booted (inside `darling shell`), as `simctl shutdown` does: its
# job in Darling's launchd is unloaded, launchd_sim gets SIGTERM and stops the device's processes, and the device's
# data stays for the next boot.
#
# usage: simshutdown.sh
#
# SIM_SHUTDOWN_TIMEOUT is how long to wait, in seconds (default: 30).
set -eu
# shellcheck source=simdevice.sh
. "$(dirname "$0")/simdevice.sh"
: "${SIM_SHUTDOWN_TIMEOUT:=30}"
sim_check_seconds SIM_SHUTDOWN_TIMEOUT "$SIM_SHUTDOWN_TIMEOUT"

if [ $# -gt 0 ]; then
	echo "usage: $0" >&2
	exit 64
fi
if ! launchctl list "$SIM_DEVICE_JOB_LABEL" >/dev/null 2>&1; then
	echo "$SIM_DEVICE_JOB_LABEL is not booted" >&2
	exit 0
fi

# It returns once launchd_sim has exited, and exits 0 even when it failed: the job going away is what counts.
launchctl unload "$SIM_DEVICE_JOB" &
sim_wait_start
while launchctl list "$SIM_DEVICE_JOB_LABEL" >/dev/null 2>&1; do
	if [ "$SIM_WAITED" -gt "$SIM_SHUTDOWN_TIMEOUT" ]; then
		echo "$SIM_DEVICE_JOB_LABEL didn't shut down in ${SIM_SHUTDOWN_TIMEOUT}s; see $SIM_DEVICE_LOG" >&2
		exit 1
	fi
	sleep 1
	sim_waited
done
wait
launchctl unload "$SIM_IOKIT_JOB" 2>/dev/null || true
echo "shut down $SIM_DEVICE_JOB_LABEL"
