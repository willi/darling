#!/bin/sh
# usage: run-all.sh [DIR] — inside `darling shell`, runs every test in DIR (the prefix's darling-tests by default).
# Each test prints one "ok"/"not ok" line per check and exits with its number of failures. This prints a line per
# test and "# F of N tests failed", and exits non-zero if any test failed, or if DIR has no test. A test failed if it
# exited non-zero or printed a "not ok" line: its exit status alone wraps at 256 failures to 0. A status above 128
# that isn't its count of "not ok" lines is the shell's for a signal: "ended by signal 6" for abort().
dir=${1:-/usr/libexec/darling-tests}
out=${TMPDIR:-/private/tmp}/run-all.$$
failed=0
ran=0
for t in "$dir"/*; do
	case ${t##*/} in run-all.sh) continue ;; esac
	[ -x "$t" ] || continue
	"$t" >"$out" 2>&1
	n=$?
	ran=$((ran + 1))
	k=$(grep -c '^not ok' "$out")
	if [ "$n" -eq 0 ] && [ "$k" -eq 0 ]; then
		echo "ok - ${t##*/}"
		continue
	fi
	failed=$((failed + 1))
	if [ "$n" -gt 128 ] && [ "$n" -ne "$k" ]; then
		echo "not ok - ${t##*/}: ended by signal $((n - 128)) ($k checks failed before)"
	else
		echo "not ok - ${t##*/}: $k checks failed (exit $n)"
	fi
	grep '^not ok' "$out" | sed 's/^/#   /'
done
rm -f "$out"
# no test at all (a wrong or empty DIR) is a failure, not "0 failed"
if [ "$ran" -eq 0 ]; then
	echo "not ok - no tests in $dir"
	failed=1
fi
echo "# $failed of $ran tests failed"
[ "$failed" -eq 0 ]
