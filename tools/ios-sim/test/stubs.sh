# stubs.sh, sourced by the tests here: the commands simboot.sh runs that a Linux host lacks.
#
# make_stubs DIR writes DIR/stub/launchctl and DIR/stub/sw_vers. launchctl keeps state as launchd does: each call goes
# in DIR/calls; load takes a job unless its Label is empty or is $LAUNCHCTL_REFUSE (then it prints the refusal and
# still exits 0, as launchctl does), list finds only the jobs it took (DIR/loaded, one label a line), unload drops one.
# sw_vers names the host's build HOSTBUILD.
make_stubs() {
	mkdir -p "$1/stub"
	cat >"$1/stub/launchctl" <<STUB
#!/bin/sh
echo "\$*" >>"$1/calls"
label() { $(command -v sed) -n 's|^<key>Label</key><string>\(.*\)</string>\$|\1|p' "\$1"; }
case \$1 in
list) grep -qxF -- "\$2" "$1/loaded" 2>/dev/null ;;
load)
	l=\$(label "\$2")
	if [ -z "\$l" ] || [ "\$l" = "\${LAUNCHCTL_REFUSE:-}" ]; then
		echo "\$l: Invalid argument" >&2
	else
		echo "\$l" >>"$1/loaded"
	fi
	;;
unload)
	l=\$(label "\$2")
	if [ -f "$1/loaded" ]; then
		grep -vxF -- "\$l" "$1/loaded" >"$1/loaded.new" || :
		mv "$1/loaded.new" "$1/loaded"
	fi
	;;
esac
STUB
	printf '#!/bin/sh\necho HOSTBUILD\n' >"$1/stub/sw_vers"
	chmod 755 "$1/stub/launchctl" "$1/stub/sw_vers"
}
