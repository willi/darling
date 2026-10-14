#!/bin/sh
# Boot an iOS Simulator device under Darling (inside `darling shell`): Apple's launchd_sim, started as CoreSimulator
# does, through launchd_sim_trampoline from a job in Darling's launchd. Programs then run in the device with
# /usr/libexec/ios-sim/simspawn.
#
# usage: SIM_RUNTIME_ROOT=<RuntimeRoot> simboot.sh
#
# simshutdown.sh shuts the device down again.
#
# SIM_DAEMONS lists the runtime's LaunchDaemons the device loads (labels; default: cfprefsd). launchd_sim starts
# each when a client first looks it up. The device's other variables are in simdevice.sh.
set -eu
: "${SIM_RUNTIME_ROOT:?set SIM_RUNTIME_ROOT to the RuntimeRoot of the runtime}"
# shellcheck source=simdevice.sh
. "$(dirname "$0")/simdevice.sh"
# logd isn't among them: os_log's buffers need memory entries darlingserver doesn't support yet
: "${SIM_DAEMONS:=com.apple.cfprefsd.xpc.daemon}"
: "${SIM_BOOT_TIMEOUT:=60}"

R="$SIM_RUNTIME_ROOT"
D="$SIM_DEVICE_DATA"
NAME="$SIM_DEVICE_JOB_LABEL"
JOB="$SIM_DEVICE_JOB"
LOG="$SIM_DEVICE_LOG"

if [ $# -gt 0 ]; then
	echo "usage: SIM_RUNTIME_ROOT=<RuntimeRoot> $0" >&2
	exit 64
fi
if launchctl list "$NAME" >/dev/null 2>&1; then
	if grep -qF "$SIM_DEVICE_BOOTED" "$LOG" 2>/dev/null; then
		echo "$NAME is already booted" >&2
		exit 0
	fi
	echo "$NAME is loaded but didn't finish booting; see $LOG. Run simshutdown.sh to try again." >&2
	exit 1
fi

mkdir -p "$D/tmp" "$D/Library/Logs" "$D/var/run/LaunchDaemons"
rm -f "$D/var/run/LaunchDaemons/"*.plist
for label in $SIM_DAEMONS; do
	ln -s "$R/System/Library/LaunchDaemons/$label.plist" "$D/var/run/LaunchDaemons/"
done

# a value as plist XML text
x() {
	printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'
}

# the environment CoreSimulator gives launchd_sim ("Self") and its jobs ("Child")
env_dict() {
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
<key>SIMULATOR_SHARED_RESOURCES_DIRECTORY</key><string>$(x "$D")</string>
<key>SIMULATOR_LOG_ROOT</key><string>$(x "$D/Library/Logs")</string>
<key>SIMULATOR_HOST_HOME</key><string>$(x "$HOME")</string>
<key>HOME</key><string>$(x "$D")</string>
<key>CFFIXED_USER_HOME</key><string>$(x "$D")</string>
<key>TMPDIR</key><string>$(x "$D/tmp/")</string>
<key>XPC_SIMULATOR_LAUNCHD_NAME</key><string>$(x "$NAME")</string>
EOF
}

# launchd_sim reads everything but the environments from inside the Bootstrap dictionary too: with Paths at the top
# level, it loads no jobs
cat >"$D/var/run/launchd_bootstrap.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Bootstrap</key><dict>
<key>Self</key><dict>$(env_dict)</dict>
<key>Child</key><dict>$(env_dict)</dict>
<key>Paths</key><array><string>$(x "$D/var/run/LaunchDaemons")</string></array>
<key>SimulatorRuntimeRoot</key><string>$(x "$R")</string>
</dict>
</dict></plist>
EOF

# the job CoreSimulator submits: the trampoline checks the device's name in and becomes launchd_sim
cat >"$JOB" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>$(x "$NAME")</string>
<key>MachServices</key><dict><key>$(x "$NAME")</key><dict><key>ResetAtClose</key><true/></dict></dict>
<key>ProgramArguments</key><array>
<string>$(x "$R/sbin/launchd_sim_trampoline")</string>
<string>$(x "$R/sbin/launchd_sim")</string>
<string>$(x "$D/var/run/launchd_bootstrap.plist")</string>
</array>
<key>RunAtLoad</key><true/>
<key>StandardOutPath</key><string>$(x "$D/Library/Logs/launchd_sim.log")</string>
<key>StandardErrorPath</key><string>$(x "$D/Library/Logs/launchd_sim.log")</string>
</dict></plist>
EOF

rm -f "$LOG"
launchctl load "$JOB"

i=0
until grep -qF "$SIM_DEVICE_BOOTED" "$LOG" 2>/dev/null; do
	i=$((i + 1))
	if [ "$i" -gt "$SIM_BOOT_TIMEOUT" ]; then
		echo "$NAME didn't finish booting in ${SIM_BOOT_TIMEOUT}s; see $LOG and $D/Library/Logs/launchd_sim.log" >&2
		echo "simshutdown.sh stops it" >&2
		exit 1
	fi
	sleep 1
done
echo "booted $NAME"
