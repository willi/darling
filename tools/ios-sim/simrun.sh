#!/bin/sh
# Run a binary from an unmodified iOS Simulator runtime under Darling (inside `darling shell`).
#
# usage: SIM_RUNTIME_ROOT=<RuntimeRoot> simrun.sh <binary path relative to RuntimeRoot> [args...]
#
# SIM_RUNTIME_ROOT is the runtime's Contents/Resources/RuntimeRoot as seen from inside Darling, e.g.
# "/Volumes/SystemRoot/<linux path>/iOS 26.5.simruntime/Contents/Resources/RuntimeRoot".
# The other variables mirror what CoreSimulator gives simulator processes; override them as needed.
set -e
: "${SIM_RUNTIME_ROOT:?set SIM_RUNTIME_ROOT to the RuntimeRoot of the runtime}"
: "${SIM_RUNTIME_VERSION:=26.5}"
: "${SIM_RUNTIME_BUILD_VERSION:=23F77}"
: "${SIM_UDID:=4C0A1A5E-0000-4000-8000-000000000001}"
: "${SIM_DEVICE_NAME:=Darling iPhone}"
: "${SIM_MODEL_IDENTIFIER:=iPhone17,3}"
: "${SIM_PRODUCT_CLASS:=D47}"
: "${SIM_DEVICE_DATA:=$HOME/Library/Developer/CoreSimulator/Devices/$SIM_UDID/data}"

[ $# -ge 1 ] || { echo "usage: SIM_RUNTIME_ROOT=<RuntimeRoot> $0 <binary> [args...]" >&2; exit 64; }
B="$1"; shift
mkdir -p "$SIM_DEVICE_DATA/tmp" "$SIM_DEVICE_DATA/Library/Logs"

exec env DYLD_ROOT_PATH="$SIM_RUNTIME_ROOT" \
  SIMULATOR_ROOT="$SIM_RUNTIME_ROOT" IPHONE_SIMULATOR_ROOT="$SIM_RUNTIME_ROOT" \
  SIMULATOR_RUNTIME_VERSION="$SIM_RUNTIME_VERSION" SIMULATOR_RUNTIME_BUILD_VERSION="$SIM_RUNTIME_BUILD_VERSION" \
  SIMULATOR_UDID="$SIM_UDID" SIMULATOR_DEVICE_NAME="$SIM_DEVICE_NAME" \
  SIMULATOR_MODEL_IDENTIFIER="$SIM_MODEL_IDENTIFIER" SIMULATOR_PRODUCT_CLASS="$SIM_PRODUCT_CLASS" \
  SIMULATOR_SHARED_RESOURCES_DIRECTORY="$SIM_DEVICE_DATA" SIMULATOR_LOG_ROOT="$SIM_DEVICE_DATA/Library/Logs" \
  SIMULATOR_HOST_HOME="$HOME" HOME="$SIM_DEVICE_DATA" CFFIXED_USER_HOME="$SIM_DEVICE_DATA" \
  TMPDIR="$SIM_DEVICE_DATA/tmp/" \
  "$SIM_RUNTIME_ROOT/$B" "$@"
