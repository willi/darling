#!/bin/sh
# simboot-fail-fast.sh: simboot.sh stops at once, saying why, when it can't write or load the device's jobs (B44: a sed
# that didn't start left the device job's Label empty, launchd refused the job, and the boot waited out its timeout).
# It runs simboot.sh on a Linux host (no Darling) with launchctl, sw_vers and sed stubbed, and a timeout far longer
# than the test allows:
#   1. sed fails, as a sed that couldn't start, for the device's name (in the bootstrap and the device job) and for
#      the trampoline's path (in the device job alone): simboot.sh exits 1 at the first file with an empty value,
#      before it loads anything;
#   2. launchd refuses the device job (launchctl load prints why and exits 0; the job isn't listed): simboot.sh exits 1
#      after that load, without waiting.
# Exits 0 when both hold, 1 otherwise, printing simboot.sh's output.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
for v in $(env | sed -n 's/^\(SIM_[A-Za-z0-9_]*\)=.*/\1/p'); do unset "$v"; done
HOME=$w/home
export HOME
mkdir -p "$w/stub" "$w/rt/System/Library/LaunchDaemons" "$w/home"
realsed=$(command -v sed)
# launchctl: each call in calls; list finds nothing (no device loaded), load prints a refusal as launchctl does
# shellcheck disable=SC2016 # the stubs' own variables
printf '#!/bin/sh\necho "$*" >>"%s/calls"\ncase $1 in list) exit 1 ;; load) echo ": Invalid argument" >&2 ;; esac\nexit 0\n' \
	"$w" >"$w/stub/launchctl"
printf '#!/bin/sh\necho HOSTBUILD\n' >"$w/stub/sw_vers"
# sed: fails, as mldr does when it can't start a process, for input that is exactly $SED_FAIL_ON
# shellcheck disable=SC2016
printf '#!/bin/sh\nin=$(cat; echo .)\nin=${in%%.}\nif [ -n "${SED_FAIL_ON:-}" ] && [ "$in" = "$SED_FAIL_ON" ]; then\n\techo "Failed to allocate stack of 65536 bytes: 17 (File exists)" >&2\n\texit 1\nfi\nprintf %%s "$in" | %s "$@"\n' \
	"$realsed" >"$w/stub/sed"
chmod 755 "$w/stub/launchctl" "$w/stub/sw_vers" "$w/stub/sed"
# the device's job label and files, as simdevice.sh names them
# shellcheck disable=SC2034 # simdevice.sh's input
SIM_DEVICE_DATA=$w/data
# shellcheck source=../simdevice.sh
. "$here/../simdevice.sh"
run() {
	rm -rf "$w/data" "$w/calls"
	mkdir -p "$w/data"
	(
		umask 022
		PATH="$w/stub:$PATH" SIM_RUNTIME_ROOT="$w/rt" SIM_DEVICE_DATA="$w/data" SIM_BOOT_TIMEOUT=600 \
			timeout 60 sh "$here/../simboot.sh" >"$w/out" 2>&1
	) && rc=0 || rc=$?
}
bad=0
fail() {
	echo "$1 (exit $rc); simboot.sh said:"
	cat "$w/out"
	bad=1
}
for c in "$SIM_DEVICE_JOB_LABEL:launchd_bootstrap.plist" "$w/rt/sbin/launchd_sim_trampoline:$SIM_DEVICE_JOB_LABEL.plist"; do
	SED_FAIL_ON=${c%:*} run
	if [ $rc != 1 ] || ! tail -1 "$w/out" | grep -q "^a program failed while simboot.sh wrote .*/${c##*:} "; then
		fail "1. sed fails for ${c%:*}: simboot.sh should exit 1 at ${c##*:}"
	elif grep -q '^load' "$w/calls" 2>/dev/null; then
		fail "1. sed fails for ${c%:*}: simboot.sh loaded a job"
	fi
done
run
if [ $rc != 1 ] || ! tail -1 "$w/out" | grep -q "^launchd didn't load .*$SIM_DEVICE_JOB_LABEL.plist"; then
	fail "2. launchd refuses the device job: simboot.sh should exit 1 after loading it"
fi
[ $bad = 0 ] && echo "simboot.sh stops at a failed job write and at a refused job"
exit $bad
