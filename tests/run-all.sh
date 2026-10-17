#!/bin/sh
# usage: run-all.sh [DIR] — inside `darling shell`, runs every test in DIR (the prefix's darling-tests by default).
# Each test prints one "ok"/"not ok" line per check and exits with its number of failures; this prints a line per
# test and exits non-zero if any check failed.
dir=${1:-/usr/libexec/darling-tests}
total=0
for t in "$dir"/*; do
	case ${t##*/} in run-all.sh) continue ;; esac
	[ -x "$t" ] || continue
	"$t" >"${TMPDIR:-/private/tmp}/run-all.$$" 2>&1
	n=$?
	if [ "$n" -eq 0 ]; then
		echo "ok - ${t##*/}"
	else
		echo "not ok - ${t##*/}: $n failed"
		grep '^not ok' "${TMPDIR:-/private/tmp}/run-all.$$" | sed 's/^/#   /'
	fi
	total=$((total + n))
done
rm -f "${TMPDIR:-/private/tmp}/run-all.$$"
echo "# $total failed"
[ "$total" -eq 0 ]
