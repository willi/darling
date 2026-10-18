# The device the ios-sim scripts work on; sourced by them. The variables mirror what CoreSimulator gives a device and
# its processes; override them as needed. SIM_RUNTIME_ROOT, the runtime's Contents/Resources/RuntimeRoot as seen from
# inside Darling, has no default.
# shellcheck shell=sh disable=SC2034
: "${SIM_RUNTIME_VERSION:=26.5}"
: "${SIM_RUNTIME_BUILD_VERSION:=23F77}"
: "${SIM_UDID:=4C0A1A5E-0000-4000-8000-000000000001}"
: "${SIM_DEVICE_NAME:=Darling iPhone}"
# the user and group the device runs as (launchd_sim and everything it starts), as CoreSimulator runs it as the Mac
# user who boots it: a Mac's first user, 501, in staff, 20 (README.md). SIM_UID=0 SIM_GID=0 run it as root, as
# Darling's launchd runs its jobs, for debugging.
: "${SIM_UID:=501}"
: "${SIM_GID:=20}"
# the device type (an Xcode .simdevicetype, e.g. iPhone 16's for iPhone17,3) as seen from inside Darling: the model,
# the screen and the environment below come from it, as CoreSimulator's do, and MobileGestalt reads its capabilities
# (the screen's shape, the artwork the device uses): UIKit can't lay out a screen without them. None: no
# capabilities, and the model and screen below are an iPhone 16's.
: "${SIM_DEVICE_TYPE:=}"
# more of the device's environment, as NAME=VALUE words
: "${SIM_EXTRA_ENVIRONMENT:=}"
if [ -n "$SIM_DEVICE_TYPE" ]; then
	sim_plist() {
		/usr/libexec/PlistBuddy -c "Print :$2" "$SIM_DEVICE_TYPE/Contents/Resources/$1" 2>/dev/null | sed 's/\.0*$//'
	}
	sim_screen=capabilities:ScreenDimensionsCapability
	: "${SIM_MODEL_IDENTIFIER:=$(sim_plist profile.plist modelIdentifier)}"
	: "${SIM_PRODUCT_CLASS:=$(sim_plist profile.plist productClass)}"
	: "${SIM_SCREEN_WIDTH:=$(sim_plist capabilities.plist "$sim_screen:main-screen-width")}"
	: "${SIM_SCREEN_HEIGHT:=$(sim_plist capabilities.plist "$sim_screen:main-screen-height")}"
	: "${SIM_SCREEN_SCALE:=$(sim_plist capabilities.plist "$sim_screen:main-screen-scale")}"
	: "${SIM_SCREEN_PITCH:=$(sim_plist capabilities.plist "$sim_screen:main-screen-pitch")}"
	# the profile's environment ("Dict {", "    NAME = value", "}"), before the caller's
	SIM_EXTRA_ENVIRONMENT="$(sim_plist profile.plist environment | sed -n 's/^ *\([A-Za-z_][A-Za-z0-9_]*\) = \(.*\)$/\1=\2/p' |
		tr '\n' ' ')$SIM_EXTRA_ENVIRONMENT"
	unset sim_screen
fi
: "${SIM_MODEL_IDENTIFIER:=iPhone17,3}"
: "${SIM_PRODUCT_CLASS:=D47}"
# the architectures the device runs apps for: without them, LaunchServices registers no app
: "${SIM_ARCHS:=x86_64}"
# the screen of an iPhone 16 (iPhone17,3), in pixels, its scale and its dot pitch (pixels per inch)
: "${SIM_SCREEN_WIDTH:=1179}"
: "${SIM_SCREEN_HEIGHT:=2556}"
: "${SIM_SCREEN_SCALE:=3}"
: "${SIM_SCREEN_PITCH:=460}"
# the SimFramebuffer framework the device's QuartzCore shows its screen with (CoreSimulator's, on a Mac); none: no
# screen, and backboardd can't start
: "${SIM_FRAMEBUFFER_FRAMEWORK:=}"
: "${SIM_DEVICE_DATA:=$HOME/Library/Developer/CoreSimulator/Devices/$SIM_UDID/data}"

# the booted device's name in Darling's bootstrap namespace, which simspawn and the runtime's libxpc look up, and the
# label of the launchd job that runs it
SIM_DEVICE_JOB_LABEL="com.apple.CoreSimulator.SimDevice.$SIM_UDID"
SIM_DEVICE_JOB="$SIM_DEVICE_DATA/var/run/$SIM_DEVICE_JOB_LABEL.plist"
# launchd_sim's own log, in the temporary state directory it makes, and its line once the device has booted
SIM_DEVICE_LOG="/private/var/tmp/$SIM_DEVICE_JOB_LABEL/launchd.log"
SIM_DEVICE_BOOTED='(system) <Notice>: exiting bootstrap mode'
# the job serving the device's IOKit (simiokitd)
SIM_IOKIT_JOB_LABEL="$SIM_DEVICE_JOB_LABEL.iokit"
SIM_IOKIT_JOB="$SIM_DEVICE_DATA/var/run/$SIM_IOKIT_JOB_LABEL.plist"
# simiokitd's log, which launchd makes when it starts simiokitd, and its line once it has registered the device's IOKit
SIM_IOKIT_LOG="$SIM_DEVICE_DATA/Library/Logs/simiokitd.log"
SIM_IOKIT_REGISTERED='simiokitd: registered'

# sim_boot_state: how far the device has booted, from one look at each log, as a word in SIM_BOOT_STATE:
# - booted: launchd_sim has left bootstrap mode, and simiokitd has registered the device's IOKit in it;
# - bootstrap: launchd_sim hasn't left bootstrap mode;
# - iokit-not-started: launchd hasn't started simiokitd;
# - iokit-unregistered: simiokitd hasn't registered; SIM_IOKIT_LAST is its log's last line (empty: none yet).
# Whether the device's job is loaded at all is launchctl's to say. It sets variables, rather than printing, so a wait
# can call it without a subshell.
sim_boot_state() {
	SIM_IOKIT_LAST=
	if ! grep -qF "$SIM_DEVICE_BOOTED" "$SIM_DEVICE_LOG" 2>/dev/null; then
		SIM_BOOT_STATE=bootstrap
	elif [ ! -e "$SIM_IOKIT_LOG" ]; then
		SIM_BOOT_STATE=iokit-not-started
	else
		sim_iokit=$(cat "$SIM_IOKIT_LOG" 2>/dev/null) || sim_iokit=
		case $sim_iokit in
		*"$SIM_IOKIT_REGISTERED"*) SIM_BOOT_STATE=booted ;;
		*)
			SIM_BOOT_STATE=iokit-unregistered
			SIM_IOKIT_LAST=${sim_iokit##*"
"}
			;;
		esac
		unset sim_iokit
	fi
}
