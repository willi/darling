#!/bin/sh
# simboot-fail-fast.sh: simboot.sh stops at once, saying why, when it can't write or load the device's jobs (B44: a sed
# that didn't start left the device job's Label empty, launchd refused the job, and the boot waited out its timeout).
# It runs simboot.sh on a Linux host (no Darling) with launchctl, sw_vers and sed stubbed, and a timeout far longer
# than the test allows:
#   1. sed fails, as a sed that couldn't start, for the device's name (in the bootstrap and the device job), for the
#      trampoline's path (in the device job alone) and for simiokitd's log (in the IOKit job alone): simboot.sh exits 1
#      at the first file with an empty value, before it loads anything;
#   2. launchd refuses the device job, or the IOKit job (launchctl load prints why and exits 0; the job isn't listed):
#      simboot.sh exits 1 right after that load, without waiting;
#   3. launchd takes both jobs and the boot doesn't come (SIM_BOOT_TIMEOUT 0): simboot.sh gets past both lookups to its
#      wait and times out there (exit 75). The other direction: a lookup that failed every good boot passes 1 and 2.
# launchctl's stub keeps state as launchd does: load takes a job unless its Label is empty or is $LAUNCHCTL_REFUSE
# (then it prints the refusal and still exits 0), list finds only jobs it took, unload drops one.
# Exits 0 when all hold, 1 otherwise, printing simboot.sh's output.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
for v in $(env | sed -n 's/^\(SIM_[A-Za-z0-9_]*\)=.*/\1/p'); do unset "$v"; done
HOME=$w/home
export HOME
mkdir -p "$w/stub" "$w/rt/System/Library/LaunchDaemons" "$w/home"
realsed=$(command -v sed)
# launchctl: each call in calls; the labels of the jobs launchd took in loaded, one a line
cat >"$w/stub/launchctl" <<STUB
#!/bin/sh
echo "\$*" >>"$w/calls"
label() { $realsed -n 's|^<key>Label</key><string>\(.*\)</string>\$|\1|p' "\$1"; }
case \$1 in
list) grep -qxF -- "\$2" "$w/loaded" 2>/dev/null ;;
load)
	l=\$(label "\$2")
	if [ -z "\$l" ] || [ "\$l" = "\${LAUNCHCTL_REFUSE:-}" ]; then
		echo "\$l: Invalid argument" >&2
	else
		echo "\$l" >>"$w/loaded"
	fi
	;;
unload)
	l=\$(label "\$2")
	if [ -f "$w/loaded" ]; then
		grep -vxF -- "\$l" "$w/loaded" >"$w/loaded.new" || :
		mv "$w/loaded.new" "$w/loaded"
	fi
	;;
esac
STUB
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
	rm -rf "$w/data" "$w/calls" "$w/loaded"
	mkdir -p "$w/data"
	(
		umask 022
		PATH="$w/stub:$PATH" SIM_RUNTIME_ROOT="$w/rt" SIM_DEVICE_DATA="$w/data" SIM_BOOT_TIMEOUT=${boot_timeout:-600} \
			timeout 60 sh "$here/../simboot.sh" >"$w/out" 2>&1
	) && rc=0 || rc=$?
}
loads() { grep -c '^load' "$w/calls" 2>/dev/null || :; }
bad=0
fail() {
	echo "$1 (exit $rc); simboot.sh said:"
	cat "$w/out"
	bad=1
}
for c in "$SIM_DEVICE_JOB_LABEL:launchd_bootstrap.plist" "$w/rt/sbin/launchd_sim_trampoline:$SIM_DEVICE_JOB_LABEL.plist" \
	"$SIM_IOKIT_LOG:$SIM_IOKIT_JOB_LABEL.plist"; do
	SED_FAIL_ON=${c%:*} run
	if [ "$rc" != 1 ] || ! tail -1 "$w/out" | grep -q "^a program failed while simboot.sh wrote .*/${c##*:} "; then
		fail "1. sed fails for ${c%:*}: simboot.sh should exit 1 at ${c##*:}"
	elif [ "$(loads)" != 0 ]; then
		fail "1. sed fails for ${c%:*}: simboot.sh loaded a job"
	fi
done
# the label launchd refuses, and how many loads simboot.sh should have made by then
for c in "$SIM_DEVICE_JOB_LABEL:1" "$SIM_IOKIT_JOB_LABEL:2"; do
	LAUNCHCTL_REFUSE=${c%:*} run
	if [ "$rc" != 1 ] || ! tail -1 "$w/out" | grep -q "^launchd didn't load .*/${c%:*}\.plist "; then
		fail "2. launchd refuses ${c%:*}: simboot.sh should exit 1 after loading it"
	elif [ "$(loads)" != "${c##*:}" ]; then
		fail "2. launchd refuses ${c%:*}: simboot.sh should stop right after that load, not after $(loads) loads"
	fi
done
boot_timeout=0 run
if [ "$rc" != 75 ] || ! tail -1 "$w/out" | grep -q "^launchd_sim didn't leave bootstrap mode"; then
	fail "3. launchd takes both jobs: simboot.sh should get to its wait and time out there (exit 75)"
fi
[ $bad = 0 ] && echo "simboot.sh stops at a failed job write and at a refused job, and goes on when both jobs load"
exit $bad
