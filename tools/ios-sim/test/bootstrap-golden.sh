#!/bin/sh
# bootstrap-golden.sh: the launchd_bootstrap.plist simboot.sh writes, against a Mac's (bootstrap.golden, a deliberate
# golden master). simboot.sh runs on a Linux host (no Darling) with launchctl and sw_vers stubbed, for an iPhone 16
# with the Mac's framebuffer, from simdevice.sh's defaults whatever the caller's environment has set; its bootstrap's
# per-machine values are replaced as the golden master's are, and every value is compared. A path that differs from
# the Mac's must be in bootstrap.allow with the value it has here ("(missing)": none): a difference not listed, a
# listed one with another value, and a listed one that no longer differs all fail. Exits 0 when they match, 1
# otherwise, printing the differences and simboot.sh's output.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
mkdir -p "$w/stub" "$w/rt/System/Library/LaunchDaemons" "$w/data" "$w/home"
# the defaults, not the caller's device: a SIM_* variable or HOME of the shell that runs the build would change it
for v in $(env | sed -n 's/^\(SIM_[A-Za-z0-9_]*\)=.*/\1/p'); do unset "$v"; done
HOME=$w/home
export HOME
# shellcheck disable=SC2016 # the stub's own $1
printf '#!/bin/sh\ncase $1 in list) exit 1 ;; esac\nexit 0\n' >"$w/stub/launchctl"
printf '#!/bin/sh\necho HOSTBUILD\n' >"$w/stub/sw_vers"
chmod 755 "$w/stub/launchctl" "$w/stub/sw_vers"
# simboot.sh writes the bootstrap, loads the device job (stubbed) and stops at its lookup, which the stub's list
# fails; the golden needs only the bootstrap
(
	umask 022
	PATH="$w/stub:$PATH" SIM_RUNTIME_ROOT="$w/rt" SIM_DEVICE_DATA="$w/data" SIM_BOOT_TIMEOUT=0 \
		SIM_DEVICE_TYPE="/Library/Developer/CoreSimulator/Profiles/DeviceTypes/iPhone 16.simdevicetype" \
		SIM_FRAMEBUFFER_FRAMEWORK=/Library/Developer/PrivateFrameworks/CoreSimulator.framework/Resources/Platforms/iphoneos/Library/PrivateFrameworks/SimFramebuffer.framework/SimFramebuffer \
		sh "$here/../simboot.sh" >"$w/simboot.out" 2>&1
) || true
# simdevice.sh's names for the device simboot.sh booted
SIM_DEVICE_DATA=$w/data
# shellcheck source=../simdevice.sh
. "$here/../simdevice.sh"
b=$SIM_DEVICE_BOOTSTRAP
[ -f "$b" ] || {
	echo "simboot.sh wrote no bootstrap:" >&2
	cat "$w/simboot.out" >&2
	exit 1
}
# the golden master's placeholders (bootstrap.golden's header), for the test's own values
awk -f "$here/plist2lines.awk" "$b" | awk -F '\t' -v OFS='\t' -v data="$w/data" -v rt="$w/rt" -v home="$w/home" \
	-v udid="$SIM_UDID" -v name="$SIM_DEVICE_NAME" '
	function rep(s, from, to,   i, r) {
		r = ""
		while ((i = index(s, from)) > 0) { r = r substr(s, 1, i - 1) to; s = substr(s, i + length(from)) }
		return r s
	}
	{
		v = $2
		v = rep(v, data, "<data>"); v = rep(v, rt, "<runtime-root>"); v = rep(v, home, "<host home>")
		v = rep(v, udid, "<uuid>"); v = rep(v, name, "<device name>"); v = rep(v, "/HOSTBUILD/", "/<host build>/")
		if ($1 ~ /\.SIMULATOR_BOOT_TIME$/ && v ~ /^[0-9]+$/) v = "<boot time>"
		print $1, v
	}' | LC_ALL=C sort >"$w/ours"
grep -v '^#' "$here/bootstrap.golden" >"$w/golden"
grep -v '^#' "$here/bootstrap.allow" | grep . >"$w/allow" || true
if awk -F '\t' -v allowf="$w/allow" -v ourf="$w/ours" '
	BEGIN {
		while ((getline l < allowf) > 0) { split(l, f, "\t"); allow[f[1]] = substr(l, length(f[1]) + 2) }
		while ((getline l < ourf) > 0) { split(l, f, "\t"); ours[f[1]] = substr(l, length(f[1]) + 2) }
	}
	{ p = $1; g = substr($0, length(p) + 2); seen[p] = 1
	  if (!(p in ours)) d[p] = "missing (Mac: " g ")"
	  else if (ours[p] != g) d[p] = "differs: " ours[p] " (Mac: " g ")" }
	END {
		for (p in ours) if (!(p in seen)) d[p] = "only here: " ours[p]
		sort = "LC_ALL=C sort"
		bad = 0
		for (p in d) {
			v = (p in ours) ? ours[p] : "(missing)"
			if (!(p in allow)) { print "not allowed: " p " " d[p] | sort; bad = 1 }
			else if (allow[p] != v) { print "allowed with another value: " p " is " v ", bootstrap.allow has " allow[p] | sort; bad = 1 }
		}
		for (p in allow) if (!(p in d)) { print "allowed but no longer different: " p | sort; bad = 1 }
		close(sort)
		exit bad
	}' "$w/golden"; then
	echo "bootstrap matches the golden master (allowed differences: $(grep -c . "$w/allow"))"
else
	echo "fix simboot.sh, or give the path its value and reason in tools/ios-sim/test/bootstrap.allow. simboot.sh said:"
	cat "$w/simboot.out"
	exit 1
fi
