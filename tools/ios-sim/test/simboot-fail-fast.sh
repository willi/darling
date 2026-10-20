#!/bin/sh
# simboot-fail-fast.sh: simboot.sh stops at once, saying why, when launchd refuses one of the device's jobs, and starts
# no program in its job files' values (B44: a sed in a $(...) that didn't start left the device job's Label empty,
# launchd refused the job, and the boot waited out its timeout). It runs simboot.sh on a Linux host (no Darling) with
# launchctl and sw_vers stubbed (stubs.sh) and a timeout far longer than the test allows:
#   1. a device whose name needs escaping, booted as is and then with sed failing, and cat failing into a pipe (each as
#      a program that couldn't start would): both times launchd takes both jobs, and simboot.sh gets past both lookups
#      to its wait and times out there (SIM_BOOT_TIMEOUT 0: exit 75); the job files have the device job's Label and the
#      name escaped, and the second boot's are the first's, byte for byte (but for the boot time). It is also the other
#      direction for 2: a lookup that failed every good boot would pass 2;
#   2. launchd refuses the device job, or the IOKit job (launchctl load prints why and exits 0; the job isn't listed):
#      simboot.sh exits 1 right after that load, without waiting.
# Exits 0 when both hold, 1 otherwise, printing simboot.sh's output.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
for v in $(env | sed -n 's/^\(SIM_[A-Za-z0-9_]*\)=.*/\1/p'); do unset "$v"; done
HOME=$w/home
export HOME
mkdir -p "$w/rt/System/Library/LaunchDaemons" "$w/home" "$w/fail"
# shellcheck source=stubs.sh
. "$here/stubs.sh"
make_stubs "$w"
# programs that couldn't start, as mldr's failure (B44) looks: sed always, cat when it writes into a pipe (a $(...))
printf '#!/bin/sh\necho "Failed to allocate stack of 65536 bytes: 17 (File exists)" >&2\nexit 1\n' >"$w/fail/sed"
# shellcheck disable=SC2016 # the stub's own variables
printf '#!/bin/sh\nif [ -p /dev/stdout ]; then\n\techo "Failed to allocate stack of 65536 bytes: 17 (File exists)" >&2\n\texit 1\nfi\nexec %s "$@"\n' \
	"$(command -v cat)" >"$w/fail/cat"
chmod 755 "$w/fail/sed" "$w/fail/cat"
# the device's job label and files, as simdevice.sh names them
# shellcheck disable=SC2034 # simdevice.sh's input
SIM_DEVICE_DATA=$w/data
# shellcheck source=../simdevice.sh
. "$here/../simdevice.sh"
run() {
	rm -rf "$w/data" "$w/calls" "$w/loaded"
	mkdir -p "$w/data"
	(
		umask 022
		PATH="${failing:+$w/fail:}$w/stub:$PATH" SIM_RUNTIME_ROOT="$w/rt" SIM_DEVICE_DATA="$w/data" \
			SIM_BOOT_TIMEOUT=${boot_timeout:-600} timeout 60 sh "$here/../simboot.sh" >"$w/out" 2>&1
	) && rc=0 || rc=$?
}
loads() { grep -c '^load' "$w/calls" 2>/dev/null || :; }
# the job files of the last run, in $w/$1, without the boot time
keep() {
	mkdir -p "$w/$1"
	for f in "$SIM_DEVICE_BOOTSTRAP" "$SIM_DEVICE_JOB" "$SIM_IOKIT_JOB"; do
		if [ -f "$f" ]; then sed 's|<key>SIMULATOR_BOOT_TIME</key><string>[0-9]*</string>||g' "$f" >"$w/$1/${f##*/}"; fi
	done
}
bad=0
fail() {
	echo "$1 (exit $rc); simboot.sh said:"
	cat "$w/out"
	bad=1
}
for failing in "" 1; do
	boot_timeout=0 SIM_DEVICE_NAME='A&B <x>' run
	keep "files$failing"
	if [ "$rc" != 75 ] || ! tail -1 "$w/out" | grep -q "^launchd_sim didn't leave bootstrap mode"; then
		fail "1. ${failing:+sed and piped cat failing: }simboot.sh should load both jobs and time out in its wait (exit 75)"
	elif ! grep -qF "<key>Label</key><string>$SIM_DEVICE_JOB_LABEL</string>" "$SIM_DEVICE_JOB" ||
		! grep -qF '<string>A&amp;B &lt;x&gt;</string>' "$SIM_DEVICE_BOOTSTRAP"; then
		fail "1. ${failing:+sed and piped cat failing: }the device job's Label, or the escaped name in the bootstrap, is wrong"
	elif [ -n "$failing" ] && ! diff -r "$w/files" "$w/files1" >"$w/diff"; then
		fail "1. sed and piped cat failing: the job files differ from a boot with them working: $(cat "$w/diff")"
	fi
done
failing=
# the label launchd refuses, and how many loads simboot.sh should have made by then
for c in "$SIM_DEVICE_JOB_LABEL:1" "$SIM_IOKIT_JOB_LABEL:2"; do
	LAUNCHCTL_REFUSE=${c%:*} run
	if [ "$rc" != 1 ] || ! tail -1 "$w/out" | grep -q "^launchd didn't load .*/${c%:*}\.plist "; then
		fail "2. launchd refuses ${c%:*}: simboot.sh should exit 1 after loading it"
	elif [ "$(loads)" != "${c##*:}" ]; then
		fail "2. launchd refuses ${c%:*}: simboot.sh should stop right after that load, not after $(loads) loads"
	fi
done
[ $bad = 0 ] && echo "simboot.sh writes its jobs without starting a program, stops at a refused job, and goes on when both load"
exit $bad
