#!/bin/sh
# bootstrap-golden.sh: the launchd_bootstrap.plist simboot.sh writes, against a Mac's (bootstrap.golden, a deliberate
# golden master). simboot.sh runs on the host (Linux or macOS, no Darling) with launchctl, sw_vers and chown stubbed,
# for an iPhone 16 with the Mac's framebuffer; its bootstrap's per-machine values are replaced as the golden master's
# are, and every value is compared. A difference fails unless bootstrap.allow names its path, and an allowed path that
# no longer differs fails too. Exits 0 when they match, 1 otherwise, printing the differences.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
mkdir -p "$w/stub" "$w/rt/System/Library/LaunchDaemons" "$w/data" "$w/home"
# shellcheck disable=SC2016 # the stub's own $1
printf '#!/bin/sh\ncase $1 in list) exit 1 ;; esac\nexit 0\n' >"$w/stub/launchctl"
printf '#!/bin/sh\necho HOSTBUILD\n' >"$w/stub/sw_vers"
printf '#!/bin/sh\nexit 0\n' >"$w/stub/chown"
chmod 755 "$w/stub/launchctl" "$w/stub/sw_vers" "$w/stub/chown"
# simboot.sh writes the bootstrap, loads its jobs (stubbed) and waits a second for a boot that doesn't come
(
	umask 022
	PATH="$w/stub:$PATH" HOME="$w/home" SIM_RUNTIME_ROOT="$w/rt" SIM_DEVICE_DATA="$w/data" SIM_BOOT_TIMEOUT=1 \
		SIM_DEVICE_TYPE="/Library/Developer/CoreSimulator/Profiles/DeviceTypes/iPhone 16.simdevicetype" \
		SIM_FRAMEBUFFER_FRAMEWORK=/Library/Developer/PrivateFrameworks/CoreSimulator.framework/Resources/Platforms/iphoneos/Library/PrivateFrameworks/SimFramebuffer.framework/SimFramebuffer \
		sh "$here/../simboot.sh" >"$w/simboot.out" 2>&1
) || true
b=$w/data/var/run/launchd_bootstrap.plist
[ -f "$b" ] || {
	echo "simboot.sh wrote no bootstrap:" >&2
	cat "$w/simboot.out" >&2
	exit 1
}
# shellcheck source=../simdevice.sh
. "$here/../simdevice.sh"
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
grep -v '^#' "$here/bootstrap.allow" | cut -f1 >"$w/allow"
awk -F '\t' -v allowf="$w/allow" -v ourf="$w/ours" '
	BEGIN {
		while ((getline l < allowf) > 0) if (l != "") allow[l] = 1
		while ((getline l < ourf) > 0) { split(l, f, "\t"); ours[f[1]] = substr(l, length(f[1]) + 2) }
	}
	{ p = $1; g = substr($0, length(p) + 2); seen[p] = 1
	  if (!(p in ours)) d[p] = "missing (Mac: " g ")"
	  else if (ours[p] != g) d[p] = "differs: " ours[p] " (Mac: " g ")" }
	END {
		for (p in ours) if (!(p in seen)) d[p] = "only here: " ours[p]
		bad = 0
		for (p in d) if (!(p in allow)) { print "not allowed: " p " " d[p]; bad = 1 }
		for (p in allow) if (!(p in d)) { print "allowed but no longer different: " p; bad = 1 }
		exit bad
	}' "$w/golden" && echo "bootstrap matches the golden master (allowed differences: $(grep -c . "$w/allow"))"
