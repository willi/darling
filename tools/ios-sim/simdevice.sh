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
: "${SIM_DEVICE_DATA:=$HOME/Library/Developer/CoreSimulator/Devices/$SIM_UDID/data}"

# the booted device's name in Darling's bootstrap namespace, which simspawn and the runtime's libxpc look up, and the
# label of the launchd job that runs it
SIM_DEVICE_JOB_LABEL="com.apple.CoreSimulator.SimDevice.$SIM_UDID"
SIM_DEVICE_JOB="$SIM_DEVICE_DATA/var/run/$SIM_DEVICE_JOB_LABEL.plist"
# launchd_sim's own log, in the temporary state directory it makes, and its line once the device has booted
SIM_DEVICE_LOG="/private/var/tmp/$SIM_DEVICE_JOB_LABEL/launchd.log"
SIM_DEVICE_BOOTED='(system) <Notice>: exiting bootstrap mode'
