#!/bin/sh
# Boot an iOS Simulator device under Darling (inside `darling shell`): Apple's launchd_sim, started as CoreSimulator
# does, through launchd_sim_trampoline from a job in Darling's launchd. Programs then run in the device with
# /usr/libexec/ios-sim/simspawn.
#
# usage: SIM_RUNTIME_ROOT=<RuntimeRoot> simboot.sh
#
# simshutdown.sh shuts the device down again.
#
# SIM_DAEMONS lists the runtime's LaunchDaemons the device loads (labels; default: cfprefsd, and logd with its
# helper; "all": every one, as CoreSimulator loads them). launchd_sim starts each when a client first looks it up, or at
# load if it says so. SIM_EXTRA_ENVIRONMENT adds NAME=VALUE pairs (space-separated) to the environment of the device's
# launchd_sim and its jobs. They go into Child, which launchd_sim sets over each job's own variables: an extra HOME,
# say, replaces an app's data container too. SIM_UID and SIM_GID are the user and group the device runs as:
# launchd_sim, and every process it starts (README.md). SIM_BOOT_TIMEOUT is how long to wait for the boot, in seconds
# (default: 60). The device's other variables are in simdevice.sh.
#
# It exits 0 once the device is booted (simdevice.sh's sim_boot_state says booted), 75 when the wait ran out with
# launchd_sim still in bootstrap mode (a slow host: a longer SIM_BOOT_TIMEOUT may do), and 1 for any other failure; the
# last line it prints says what is missing.
set -eu
: "${SIM_RUNTIME_ROOT:?set SIM_RUNTIME_ROOT to the RuntimeRoot of the runtime}"
# shellcheck source=simdevice.sh
. "$(dirname "$0")/simdevice.sh"
: "${SIM_DAEMONS:=com.apple.cfprefsd.xpc.daemon com.apple.logd com.apple.logd_helper}"
: "${SIM_BOOT_TIMEOUT:=60}"
sim_check_seconds SIM_BOOT_TIMEOUT "$SIM_BOOT_TIMEOUT"

R="$SIM_RUNTIME_ROOT"
D="$SIM_DEVICE_DATA"
NAME="$SIM_DEVICE_JOB_LABEL"
JOB="$SIM_DEVICE_JOB"
LOG="$SIM_DEVICE_LOG"

if [ $# -gt 0 ]; then
	echo "usage: SIM_RUNTIME_ROOT=<RuntimeRoot> $0" >&2
	exit 64
fi
# simasuid takes 0 to 2147483647, as Darling keeps ids; here, before a job that would fail out of sight
for id in "$SIM_UID" "$SIM_GID"; do
	case $id in
	'' | *[!0-9]* | ???????????*)
		echo "SIM_UID and SIM_GID must be numbers from 0 to 2147483647: $SIM_UID, $SIM_GID" >&2
		exit 64
		;;
	esac
	if [ "$id" -gt 2147483647 ]; then
		echo "SIM_UID and SIM_GID must be numbers from 0 to 2147483647: $SIM_UID, $SIM_GID" >&2
		exit 64
	fi
done
if launchctl list "$NAME" >/dev/null 2>&1; then
	sim_boot_state
	if [ "$SIM_BOOT_STATE" = booted ]; then
		echo "$NAME is already booted" >&2
		exit 0
	fi
	echo "$NAME is loaded but didn't finish booting ($SIM_BOOT_STATE); see $LOG and $SIM_IOKIT_LOG." \
		"Run simshutdown.sh to try again." >&2
	exit 1
fi
# launchctl skips a job plist that group or others can write ("Dubious permissions"), and the jobs below get this
# shell's umask: a darling shell starts with launchd's, which an older darling took from whatever started the prefix
if [ $(($(umask) & 022)) -ne $((022)) ]; then
	echo "the umask is $(umask): launchctl skips job plists that group or others can write. Run simboot.sh with umask" \
		"022; if a new darling shell has $(umask) too, shut the prefix down (darling shutdown) and start it again" >&2
	exit 1
fi

mkdir -p "$D/tmp" "$D/Library/Logs" "$D/var/run/LaunchDaemons"
rm -f "$D/var/run/LaunchDaemons/"*.plist
if [ "$SIM_DAEMONS" = all ]; then
	SIM_DAEMONS=$(cd "$R/System/Library/LaunchDaemons" && for f in *.plist; do printf '%s ' "${f%.plist}"; done)
fi
for label in $SIM_DAEMONS; do
	ln -s "$R/System/Library/LaunchDaemons/$label.plist" "$D/var/run/LaunchDaemons/"
done

# a value as plist XML text
x() {
	printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'
}

# the environment CoreSimulator gives launchd_sim ("Self": all of it) and its jobs ("Child"). launchd_sim sets Child
# over a job's own EnvironmentVariables
env_child() {
	cat <<EOF
<key>DYLD_ROOT_PATH</key><string>$(x "$R")</string>
<key>SIMULATOR_ROOT</key><string>$(x "$R")</string>
<key>IPHONE_SIMULATOR_ROOT</key><string>$(x "$R")</string>
<key>SIMULATOR_RUNTIME_VERSION</key><string>$(x "$SIM_RUNTIME_VERSION")</string>
<key>SIMULATOR_RUNTIME_BUILD_VERSION</key><string>$(x "$SIM_RUNTIME_BUILD_VERSION")</string>
<key>SIMULATOR_UDID</key><string>$(x "$SIM_UDID")</string>
<key>SIMULATOR_DEVICE_NAME</key><string>$(x "$SIM_DEVICE_NAME")</string>
<key>SIMULATOR_MODEL_IDENTIFIER</key><string>$(x "$SIM_MODEL_IDENTIFIER")</string>
<key>SIMULATOR_PRODUCT_CLASS</key><string>$(x "$SIM_PRODUCT_CLASS")</string>
<key>SIMULATOR_ARCHS</key><string>$(x "$SIM_ARCHS")</string>
<key>SIMULATOR_SHARED_RESOURCES_DIRECTORY</key><string>$(x "$D")</string>
<key>SIMULATOR_LOG_ROOT</key><string>$(x "$D/Library/Logs")</string>
<key>SIMULATOR_HOST_HOME</key><string>$(x "$HOME")</string>
<key>XPC_SIMULATOR_LAUNCHD_NAME</key><string>$(x "$NAME")</string>
<key>SIMULATOR_MAINSCREEN_WIDTH</key><string>$(x "$SIM_SCREEN_WIDTH")</string>
<key>SIMULATOR_MAINSCREEN_HEIGHT</key><string>$(x "$SIM_SCREEN_HEIGHT")</string>
<key>SIMULATOR_MAINSCREEN_SCALE</key><string>$(x "$SIM_SCREEN_SCALE")</string>
<key>SIMULATOR_MAINSCREEN_PITCH</key><string>$(x "$SIM_SCREEN_PITCH")</string>
<key>IOS_SIMULATOR_IOKITSIMD</key><string>1</string>
EOF
	for v in $SIM_EXTRA_ENVIRONMENT; do
		echo "<key>$(x "${v%%=*}")</key><string>$(x "${v#*=}")</string>"
	done
	if [ -n "$SIM_DEVICE_TYPE" ]; then
		echo "<key>SIMULATOR_CAPABILITIES</key><string>$(x "$SIM_DEVICE_TYPE/Contents/Resources/capabilities.plist")</string>"
	fi
	if [ -n "$SIM_FRAMEBUFFER_FRAMEWORK" ]; then
		echo "<key>SIMULATOR_FRAMEBUFFER_FRAMEWORK</key><string>$(x "$SIM_FRAMEBUFFER_FRAMEWORK")</string>"
	fi
}

# and its jobs' defaults ("ChildDefaults"), which a job's own EnvironmentVariables override. The home and temporary
# directory are defaults in CoreSimulator's launchd_bootstrap.plist: an app's job keeps the data container's, which
# RunningBoard gives it (from installd's record, through FrontBoard)
env_defaults() {
	cat <<EOF
<key>CFFIXED_USER_HOME</key><string>$(x "$D")</string>
<key>HOME</key><string>$(x "$D")</string>
<key>TMPDIR</key><string>$(x "$D/tmp/")</string>
EOF
}

# a device booted before keeps its job files, and with them their modes: they're written anew with this umask
rm -f "$D/var/run/launchd_bootstrap.plist" "$JOB" "$SIM_IOKIT_JOB"

# launchd_sim reads everything but the environments from inside the Bootstrap dictionary too: with Paths at the top
# level, it loads no jobs. In Self, Child comes after the defaults: a plist key given twice (an extra HOME) has its last
# value, so an extra is launchd_sim's own as it is its jobs'
cat >"$D/var/run/launchd_bootstrap.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Bootstrap</key><dict>
<key>Self</key><dict>$(env_defaults)$(env_child)</dict>
<key>Child</key><dict>$(env_child)</dict>
<key>ChildDefaults</key><dict>$(env_defaults)</dict>
<key>Paths</key><array><string>$(x "$D/var/run/LaunchDaemons")</string></array>
<key>SimulatorRuntimeRoot</key><string>$(x "$R")</string>
</dict>
</dict></plist>
EOF

# the job CoreSimulator submits (to the domain of the user who boots the device, so launchd_sim runs as that user):
# the trampoline checks the device's name in and becomes launchd_sim. Darling's launchd runs its jobs as root, so a
# device as anyone else starts through simasuid.
as_user=
if [ "$SIM_UID" != 0 ] || [ "$SIM_GID" != 0 ]; then
	as_user="<string>/usr/libexec/ios-sim/simasuid</string><string>$SIM_UID</string><string>$SIM_GID</string>"
fi
cat >"$JOB" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>$(x "$NAME")</string>
<key>MachServices</key><dict><key>$(x "$NAME")</key><dict><key>ResetAtClose</key><true/></dict></dict>
<key>ProgramArguments</key><array>$as_user
<string>$(x "$R/sbin/launchd_sim_trampoline")</string>
<string>$(x "$R/sbin/launchd_sim")</string>
<string>$(x "$D/var/run/launchd_bootstrap.plist")</string>
</array>
<key>RunAtLoad</key><true/>
<key>StandardOutPath</key><string>$(x "$D/Library/Logs/launchd_sim.log")</string>
<key>StandardErrorPath</key><string>$(x "$D/Library/Logs/launchd_sim.log")</string>
</dict></plist>
EOF

# Darling has no kernel IOKit for the device's IOKit to talk to, as a Mac does: with IOS_SIMULATOR_IOKITSIMD set, the
# device's IOKit looks up com.apple.iokitsimd instead, which simiokitd registers in the device once it is up.
# launchd_sim takes a host client's request only from its own user (README.md), so simiokitd runs as the device does.
cat >"$SIM_IOKIT_JOB" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>$(x "$SIM_IOKIT_JOB_LABEL")</string>
<key>ProgramArguments</key><array>$as_user<string>/usr/libexec/ios-sim/simiokitd</string></array>
<key>EnvironmentVariables</key><dict>
<key>SIM_RUNTIME_ROOT</key><string>$(x "$R")</string>
<key>SIM_UDID</key><string>$(x "$SIM_UDID")</string>
<key>SIM_BOOT_TIMEOUT</key><string>$(x "$SIM_BOOT_TIMEOUT")</string>
</dict>
<key>RunAtLoad</key><true/>
<key>StandardErrorPath</key><string>$(x "$SIM_IOKIT_LOG")</string>
</dict></plist>
EOF

rm -f "$LOG" "$SIM_IOKIT_LOG"
launchctl load "$JOB"
launchctl load "$SIM_IOKIT_JOB"

# booted once launchd_sim has left bootstrap mode and simiokitd has registered the device's IOKit in it (a daemon that
# asks IOKit before then gets no answer). The timeout is in seconds, not passes: each grep is a process start, which a
# loaded host makes slow.
sim_wait_start
gone=0
while :; do
	# one look at each half, for the wait and for what a timeout says is missing (the log is removed above)
	sim_boot_state
	[ "$SIM_BOOT_STATE" = booted ] && break
	# simiokitd tries for SIM_BOOT_TIMEOUT too, and launchd doesn't start it again: once it has exited without
	# registering (no PID, twice: launchd makes its log before it notes the PID), no wait brings the IOKit
	if [ "$SIM_BOOT_STATE" = iokit-unregistered ] && ! launchctl list "$SIM_IOKIT_JOB_LABEL" 2>/dev/null | grep -q '"PID" = '; then
		gone=$((gone + 1))
	else
		gone=0
	fi
	if [ "$gone" -ge 2 ]; then
		echo "$NAME didn't finish booting; see $LOG, $D/Library/Logs/launchd_sim.log and $SIM_IOKIT_LOG" >&2
		echo "simshutdown.sh stops it" >&2
		echo "launchd_sim is up, but simiokitd exited without registering the device's IOKit" \
			"(simiokitd.log: ${SIM_IOKIT_LAST:-empty})" >&2
		exit 1
	fi
	if [ "$SIM_WAITED" -gt "$SIM_BOOT_TIMEOUT" ]; then
		echo "$NAME didn't finish booting in ${SIM_BOOT_TIMEOUT}s; see $LOG, $D/Library/Logs/launchd_sim.log and $SIM_IOKIT_LOG" >&2
		echo "simshutdown.sh stops it" >&2
		# what is missing, last (callers print the last line)
		case $SIM_BOOT_STATE in
		bootstrap)
			echo "launchd_sim didn't leave bootstrap mode" >&2
			exit 75
			;;
		iokit-not-started)
			if launchctl list "$SIM_IOKIT_JOB_LABEL" >/dev/null 2>&1; then
				echo "launchd_sim is up, but simiokitd's job is loaded and launchd never ran it" >&2
			else
				echo "launchd_sim is up, but simiokitd's job isn't loaded" >&2
			fi
			;;
		*)
			echo "launchd_sim is up, but simiokitd never registered the device's IOKit" \
				"(simiokitd.log: ${SIM_IOKIT_LAST:-empty: still trying})" >&2
			;;
		esac
		exit 1
	fi
	sleep 1
	sim_waited
done
echo "booted $NAME"
