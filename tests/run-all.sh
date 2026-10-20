#!/bin/sh
# usage: run-all.sh [DIR] — inside `darling shell`, runs every test in DIR (the prefix's darling-tests by default).
# Each test prints one "ok"/"not ok" line per check and exits with its number of failures; this prints a line per
# test and exits non-zero if any check failed, or if DIR has no test. A test that a signal ends counts as
# 128 + the signal failed (the shell's status for it): "not ok - NAME: 134 failed" for abort().
dir=${1:-/usr/libexec/darling-tests}
total=0
ran=0
for t in "$dir"/*; do
	case ${t##*/} in run-all.sh) continue ;; esac
	[ -x "$t" ] || continue
	"$t" >"${TMPDIR:-/private/tmp}/run-all.$$" 2>&1
	n=$?
	ran=$((ran + 1))
	if [ "$n" -eq 0 ]; then
		echo "ok - ${t##*/}"
	else
		echo "not ok - ${t##*/}: $n failed"
		grep '^not ok' "${TMPDIR:-/private/tmp}/run-all.$$" | sed 's/^/#   /'
	fi
	total=$((total + n))
done
rm -f "${TMPDIR:-/private/tmp}/run-all.$$"
# no test at all (a wrong or empty DIR) is a failure, not "0 failed"
if [ "$ran" -eq 0 ]; then
	echo "not ok - no tests in $dir"
	total=1
fi
echo "# $total failed"
[ "$total" -eq 0 ]
