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
# (default: 60). SIM_DYLD_SHARED_CACHE_DIR is the directory dyld_sim looks in for a cache after the runtime root's
# (default: CoreSimulator's for the runtime and the host's build; empty: none). The device's other variables are in
# simdevice.sh.
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
BOOTSTRAP="$SIM_DEVICE_BOOTSTRAP"
BOOT_TIME=$(date +%s)

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

# The job files below are heredocs, and each value in them is a $(...): a program that fails there (a sed that doesn't
# start) leaves its value empty, and set -e doesn't see it. One such left the device job's Label empty, and launchd
# refused the job, so the boot waited out its timeout for a launchd_sim that never started (B44). Each program that
# runs in one marks its failure in this file, and every job file is checked for it once it is written.
SUBST_FAILED="$D/var/run/simboot-subst-failed"
rm -f "$SUBST_FAILED"

# fails the boot when a program in a $(...) failed while simboot.sh wrote $1
check_written() {
	if [ -e "$SUBST_FAILED" ]; then
		rm -f "$SUBST_FAILED"
		echo "a program failed while simboot.sh wrote $1 (its message is above); not loading the device from it" >&2
		exit 1
	fi
}

# a value as plist XML text
x() {
	printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g' || : >"$SUBST_FAILED"
}

# the directory of the device's dyld shared cache, as CoreSimulator names it: the runtime's cache for the host's macOS
# build (sw_vers's; Darling's is "Darling"). dyld_sim takes a cache from the runtime root's own
# System/Library/Caches/com.apple.dyld first, then from this directory, and with this set never from the prefix's
# /System/Library/Caches (dyld's CacheFinder). Darling builds no cache here, so a device has one only if its runtime
# root carries one. SIM_DYLD_SHARED_CACHE_DIR set and empty leaves the variable out, so dyld_sim looks in the prefix's
# /System/Library/Caches after the runtime root's
if [ -z "${SIM_DYLD_SHARED_CACHE_DIR+set}" ]; then
	host_build=$(sw_vers -buildVersion) || host_build=
	if [ -z "$host_build" ]; then
		echo "sw_vers -buildVersion gave no build, which the device's dyld cache directory is named after" \
			"(SIM_DYLD_SHARED_CACHE_DIR sets the directory)" >&2
		exit 1
	fi
	SIM_DYLD_SHARED_CACHE_DIR=/Library/Developer/CoreSimulator/Caches/dyld/$host_build/$SIM_RUNTIME_IDENTIFIER.$SIM_RUNTIME_BUILD_VERSION/
fi

# the environments CoreSimulator gives launchd_sim's jobs. "Child" is this one: launchd_sim sets it over a job's own
# EnvironmentVariables. launchd_sim's own environment, "Self", is Child and the defaults below together, as a Mac's
# (whose Self lacks one key of its Child's, VM_KERNEL_PAGE_SIZE_4K, which is in Child alone here too). The keys are a
# Mac's, with its values and paths (README.md):
# - IPHONE_SHARED_RESOURCES_DIRECTORY, the data directory: launchd_sim reads it from Child and replaces the /var/mobile
#   a job's paths start with by it (WorkingDirectory, KeepAlive PathState, WatchPaths, QueueDirectories). Without it
#   the replacement is "(null)": a job whose working directory is under /var/mobile can't start, and path conditions
#   never hold. syslogd and libAccessibility read it too.
# - SIMULATOR_EXTENDED_DISPLAY_PROPERTIES and IPHONE_TVOUT_EXTENDED_PROPERTIES, the extended display's plist:
#   backboardd, BackBoardServices and UIKitCore read the first.
# - SIMULATOR_AUDIO_SETTINGS_PATH (AudioToolbox, AudioSession) and SIMULATOR_AUDIO_DEVICES_PLIST_PATH, the audio
#   settings and devices in the device's var/run. simboot writes neither: a Mac's CoreSimulator writes the devices
#   plist before the boot, naming the Mac's own audio devices, which a Darling host doesn't have.
# - SIMULATOR_MEMORY_WARNINGS (libdispatch), a file simboot makes empty, as a Mac's CoreSimulator does: a simulator's
#   libdispatch opens it to watch for memory warnings: with no file the open fails, and with no key its
#   memory-pressure sources watch nothing.
# - SIMULATOR_VERSION_INFO (libsystem_sim_platform, libsystem_trace): CoreSimulator's version (the oracle's), the
#   device, the runtime and the device type (simdevice.sh's SIM_CORESIMULATOR_VERSION and SIM_DEVICE_TYPE_NAME).
# - LS_ENABLE_BUNDLE_LOCALIZATION_CACHING (CoreServices: LaunchServices).
# - SIMULATOR_BOOT_TIME, when simboot writes the bootstrap (seconds since 1970). Nothing in the runtime reads it.
# - VM_KERNEL_PAGE_SIZE_4K, in Child alone. Nothing in the runtime reads it: it's here to match a Mac's environment.
env_child() {
	cat <<EOF || : >"$SUBST_FAILED"
<key>DYLD_ROOT_PATH</key><string>$(x "$R")</string>
<key>PATH</key><string>$(x "$R/usr/bin:$R/bin:$R/usr/sbin:$R/sbin:$R/usr/local/bin")</string>
<key>SIMULATOR_ROOT</key><string>$(x "$R")</string>
<key>IPHONE_SIMULATOR_ROOT</key><string>$(x "$R")</string>
<key>SIMULATOR_RUNTIME_VERSION</key><string>$(x "$SIM_RUNTIME_VERSION")</string>
<key>SIMULATOR_RUNTIME_BUILD_VERSION</key><string>$(x "$SIM_RUNTIME_BUILD_VERSION")</string>
<key>SIMULATOR_UDID</key><string>$(x "$SIM_UDID")</string>
<key>SIMULATOR_DEVICE_NAME</key><string>$(x "$SIM_DEVICE_NAME")</string>
<key>SIMULATOR_MODEL_IDENTIFIER</key><string>$(x "$SIM_MODEL_IDENTIFIER")</string>
<key>SIMULATOR_PRODUCT_CLASS</key><string>$(x "$SIM_PRODUCT_CLASS")</string>
<key>SIMULATOR_ARCHS</key><string>$(x "$SIM_ARCHS")</string>
<key>IPHONE_SHARED_RESOURCES_DIRECTORY</key><string>$(x "$D")</string>
<key>SIMULATOR_SHARED_RESOURCES_DIRECTORY</key><string>$(x "$D")</string>
<key>SIMULATOR_LOG_ROOT</key><string>$(x "$D/Library/Logs")</string>
<key>SIMULATOR_HOST_HOME</key><string>$(x "$HOME")</string>
<key>XPC_SIMULATOR_LAUNCHD_NAME</key><string>$(x "$NAME")</string>
<key>SIMULATOR_MAINSCREEN_WIDTH</key><string>$(x "$SIM_SCREEN_WIDTH")</string>
<key>SIMULATOR_MAINSCREEN_HEIGHT</key><string>$(x "$SIM_SCREEN_HEIGHT")</string>
<key>SIMULATOR_MAINSCREEN_SCALE</key><string>$(x "$SIM_SCREEN_SCALE")</string>
<key>SIMULATOR_MAINSCREEN_PITCH</key><string>$(x "$SIM_SCREEN_PITCH")</string>
<key>IOS_SIMULATOR_IOKITSIMD</key><string>1</string>
<key>SIMULATOR_EXTENDED_DISPLAY_PROPERTIES</key><string>$(x "$D/Library/Application Support/Simulator/extended_display.plist")</string>
<key>IPHONE_TVOUT_EXTENDED_PROPERTIES</key><string>$(x "$D/Library/Application Support/Simulator/extended_display.plist")</string>
<key>SIMULATOR_AUDIO_SETTINGS_PATH</key><string>$(x "$D/var/run/simulatoraudio/audiosettings.plist")</string>
<key>SIMULATOR_AUDIO_DEVICES_PLIST_PATH</key><string>$(x "$D/var/run/com.apple.coresimulator.audio.plist")</string>
<key>SIMULATOR_MEMORY_WARNINGS</key><string>$(x "$D/var/run/memory_warning_simulation")</string>
<key>SIMULATOR_VERSION_INFO</key><string>$(x "CoreSimulator $SIM_CORESIMULATOR_VERSION - Device: $SIM_DEVICE_NAME ($SIM_UDID) - Runtime: iOS $SIM_RUNTIME_VERSION ($SIM_RUNTIME_BUILD_VERSION) - DeviceType: $SIM_DEVICE_TYPE_NAME")</string>
<key>LS_ENABLE_BUNDLE_LOCALIZATION_CACHING</key><string>1</string>
<key>SIMULATOR_BOOT_TIME</key><string>$BOOT_TIME</string>
EOF
	for v in $SIM_EXTRA_ENVIRONMENT; do
		echo "<key>$(x "${v%%=*}")</key><string>$(x "${v#*=}")</string>"
	done
	if [ -n "$SIM_DYLD_SHARED_CACHE_DIR" ]; then
		echo "<key>DYLD_SHARED_CACHE_DIR</key><string>$(x "$SIM_DYLD_SHARED_CACHE_DIR")</string>"
	fi
	if [ -n "$SIM_DEVICE_TYPE" ]; then
		echo "<key>SIMULATOR_CAPABILITIES</key><string>$(x "$SIM_DEVICE_TYPE/Contents/Resources/capabilities.plist")</string>"
	fi
	if [ -n "$SIM_FRAMEBUFFER_FRAMEWORK" ]; then
		echo "<key>SIMULATOR_FRAMEBUFFER_FRAMEWORK</key><string>$(x "$SIM_FRAMEBUFFER_FRAMEWORK")</string>"
	fi
}

# "ChildDefaults", the jobs' defaults: a job's own EnvironmentVariables override them. The home and temporary directory
# are defaults in CoreSimulator's launchd_bootstrap.plist: an app's job keeps the data container's, which RunningBoard
# gives it (from installd's record, through FrontBoard), and a job without its own (a daemon's) gets the device's data.
# dyld's fallback paths are the runtime's, as CoreSimulator's are. Each key is in the dict CoreSimulator puts it in
env_defaults() {
	cat <<EOF || : >"$SUBST_FAILED"
<key>CFFIXED_USER_HOME</key><string>$(x "$D")</string>
<key>DYLD_FALLBACK_FRAMEWORK_PATH</key><string>$(x "$R/System/Library/Frameworks")</string>
<key>DYLD_FALLBACK_LIBRARY_PATH</key><string>$(x "$R/usr/lib")</string>
<key>HOME</key><string>$(x "$D")</string>
<key>TMPDIR</key><string>$(x "$D/tmp/")</string>
EOF
}

# a device booted before keeps its job files, and with them their modes: they're written anew with this umask
rm -f "$BOOTSTRAP" "$JOB" "$SIM_IOKIT_JOB"
# the memory-warning file a Mac's CoreSimulator makes before the boot, empty
: >"$D/var/run/memory_warning_simulation"

# launchd_sim reads everything but the environments from inside the Bootstrap dictionary too: with Paths at the top
# level, it loads no jobs. In Self, Child comes after the defaults: a plist key given twice (an extra HOME) has its last
# value, so an extra is launchd_sim's own as it is its jobs'
cat >"$BOOTSTRAP" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Bootstrap</key><dict>
<key>Self</key><dict>$(env_defaults)$(env_child)</dict>
<key>Child</key><dict>$(env_child)<key>VM_KERNEL_PAGE_SIZE_4K</key><string>1</string></dict>
<key>ChildDefaults</key><dict>$(env_defaults)</dict>
<key>Paths</key><array><string>$(x "$D/var/run/LaunchDaemons")</string></array>
<key>SimulatorRuntimeRoot</key><string>$(x "$R")</string>
</dict>
</dict></plist>
EOF
check_written "$BOOTSTRAP"

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
<string>$(x "$BOOTSTRAP")</string>
</array>
<key>RunAtLoad</key><true/>
<key>StandardOutPath</key><string>$(x "$D/Library/Logs/launchd_sim.log")</string>
<key>StandardErrorPath</key><string>$(x "$D/Library/Logs/launchd_sim.log")</string>
</dict></plist>
EOF
check_written "$JOB"

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
check_written "$SIM_IOKIT_JOB"

rm -f "$LOG" "$SIM_IOKIT_LOG"
# launchctl load exits 0 when launchd refuses a job (it only prints why), so each job is looked up after it: a
# device that can't start fails here, not at the end of the wait
# (list's own errors aren't hidden: a list that couldn't start says so above, and the job may be loaded after all)
launchctl load "$JOB"
if ! launchctl list "$NAME" >/dev/null; then
	echo "launchd didn't load $JOB (launchctl's message is above); simshutdown.sh stops $NAME if it is loaded" >&2
	exit 1
fi
# an IOKit job left loaded by an earlier boot that stopped here would make the load "Already loaded" and the lookup
# below pass, and the boot would then wait for a simiokitd that never runs: unload it first (it has the same label)
launchctl unload "$SIM_IOKIT_JOB" 2>/dev/null || true
launchctl load "$SIM_IOKIT_JOB"
if ! launchctl list "$SIM_IOKIT_JOB_LABEL" >/dev/null; then
	echo "launchd didn't load $SIM_IOKIT_JOB (launchctl's message is above); simshutdown.sh stops $NAME" >&2
	exit 1
fi

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
