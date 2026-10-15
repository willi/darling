# The device the ios-sim scripts work on; sourced by them. The variables mirror what CoreSimulator gives a device and
# its processes; override them as needed. SIM_RUNTIME_ROOT, the runtime's Contents/Resources/RuntimeRoot as seen from
# inside Darling, has no default.
# shellcheck shell=sh disable=SC2034
: "${SIM_RUNTIME_VERSION:=26.5}"
: "${SIM_RUNTIME_BUILD_VERSION:=23F77}"
: "${SIM_UDID:=4C0A1A5E-0000-4000-8000-000000000001}"
: "${SIM_DEVICE_NAME:=Darling iPhone}"
: "${SIM_MODEL_IDENTIFIER:=iPhone17,3}"
: "${SIM_PRODUCT_CLASS:=D47}"
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
