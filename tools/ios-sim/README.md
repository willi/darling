# Running an iOS Simulator runtime under Darling

Apple's iOS Simulator runtimes (`iOS <version>.simruntime`) are x86_64 Mach-O user space that runs on the macOS host
kernel through `dyld_sim`. Under Darling they run unmodified: Darling's `libsystem_kernel`, `libsystem_platform` and
`libsystem_pthread` play the host libraries that the runtime's `libsystem_sim_*` shims re-export.

Status, with the iOS 26.5 runtime:
- command-line tools such as `usr/bin/tab2space` run to completion;
- Foundation/XPC clients such as `usr/bin/defaults read` run to completion;
- a device boots: Apple's `launchd_sim` runs the runtime's daemons and the programs `simspawn` asks for. `defaults
  write` and `defaults read` go through the device's own `cfprefsd`, which `launchd_sim` starts on demand;
- a device shuts down: `launchd_sim` stops its daemons and exits, and the device boots again with its data;
- `os_log` works: the device's `logd` collects the messages of the programs in the device and stores them, and a
  `.logarchive` reader reads them on Linux;
- an app installs and runs: the device shows SpringBoard's home screen, `siminstall` installs an app through installd
  and opens it through SpringBoard, and the app's UIKit draws its window. Images the app or SpringBoard draw from
  asset catalogs (icons, an app's artwork) don't show yet.

## Usage

1. Build Darling with the `ios_sim` component (`-DCOMPONENTS=stock,ios_sim`, or `all`). It installs `simrun.sh`,
   `simboot.sh`, `simshutdown.sh`, `simspawn` and `simasuid` in the prefix's `/usr/libexec/ios-sim`.
2. Extract a runtime (e.g. from Xcode's `iOS_<version>_Simulator_Runtime.dmg`). Do not modify its files: its code
   signatures should keep verifying.
3. Inside `darling shell`:

   ```sh
   SIM_RUNTIME_ROOT="/Volumes/SystemRoot/<path>/iOS 26.5.simruntime/Contents/Resources/RuntimeRoot" \
     /usr/libexec/ios-sim/simrun.sh usr/bin/tab2space < input.txt
   ```

`simrun.sh` sets the environment CoreSimulator gives simulator processes (`SIMULATOR_*`, `HOME`, `TMPDIR`, ...).
Device data defaults to `~/Library/Developer/CoreSimulator/Devices/<udid>/data`.

`simrun.sh` runs a program on its own, outside any device: with no `cfprefsd` to talk to, `defaults read` works but
`defaults write` fails. To run it in a booted device instead, as `simctl spawn` does:

```sh
export SIM_RUNTIME_ROOT=...
/usr/libexec/ios-sim/simboot.sh
/usr/libexec/ios-sim/simspawn usr/bin/defaults write com.example.test greeting hello
/usr/libexec/ios-sim/simspawn usr/bin/defaults read com.example.test greeting
/usr/libexec/ios-sim/simshutdown.sh
```

`simboot.sh` boots the device as CoreSimulator does: a job in Darling's `launchd` runs the runtime's
`launchd_sim_trampoline`, which hands the device's bootstrap port to `launchd_sim`. The device loads the daemons in
`SIM_DAEMONS` (`cfprefsd`, `logd` and `logd_helper` by default). `simspawn` passes `SIMCTL_CHILD_*` variables to the program, as
`simctl spawn` does; `SIMCTL_CHILD_OS_ACTIVITY_DT_MODE=1` prints its `os_log` messages on stderr.
The device's `launchd_bootstrap.plist` gives launchd_sim its own environment (`Self`) and its jobs': `Child`, which
launchd_sim sets over a job's own `EnvironmentVariables`, and `ChildDefaults`, which they override. As in
CoreSimulator's, `HOME`, `CFFIXED_USER_HOME` and `TMPDIR` are defaults, so an app keeps the data container's that
RunningBoard puts in its job.
`simboot.sh` needs umask 022 (or one that also keeps group and others from writing): `launchctl` skips job plists that
group or others can write. A `darling shell` has `launchd`'s umask, which `darling` starts with at least 022.
`launchd_sim` logs to `/private/var/tmp/com.apple.CoreSimulator.SimDevice.<udid>/launchd.log`.
The device runs as `SIM_UID` and `SIM_GID` (501 and 20 by default, a Mac's first user and its staff group):
CoreSimulator submits the job to the domain of the user who boots the device, so `launchd_sim` and everything it
starts run as that user, never as root. Darling's `launchd` runs its jobs as root, so `simboot.sh` starts the
trampoline through `simasuid`, which sets the groups (to just the gid), the gid and the uid, and execs it. Some of the
runtime takes other paths as root: libAccessibility, for one, asks backboardd over IPC for each preference when
`geteuid()` is 0, and backboardd, which serves that IPC, then waits on itself. `SIM_UID=0 SIM_GID=0` keep the old
behaviour, for debugging. Whatever else talks to the device (a per-user launchd domain, `user/<uid>`) should use the
same `SIM_UID`. `launchd_sim` takes a request from a host client (`launch_sim_spawn`, `launch_sim_register_endpoint`)
only when the caller's euid is its own uid, root included, as CoreSimulator's clients run as the same Mac user as the
device: so `simboot.sh` starts `simiokitd` through `simasuid` too, and `simspawn` must run as the device's user,
`simasuid $SIM_UID $SIM_GID simspawn <program>` (as root against a device at 501 it gets `Operation not permitted`). Darling keeps the ids in darlingserver, and they don't reach everywhere:
- `stat()` reports every file as owned by the caller, so a device at 501 sees its root-owned data as its own;
- `proc_pidinfo()`/sysctl report the caller's uid for every process;
- `setuid()` needs no privilege: a process at 501 can become root again (a job whose plist says `UserName root`
  could), where XNU refuses;
- the prefix's users are root and the user who created it, so the directory has no user 501: libc's
  `getgroups()`, which answers a user's groups from the directory, fails at 501.
`simshutdown.sh` unloads the device's job, as `simctl shutdown` does: `launchd` sends `launchd_sim` SIGTERM, and
`launchd_sim` stops the device's processes and exits. The device's variables (`SIM_UDID` and so on) are in
`simdevice.sh`.

### Installing and opening an app

A device shows a home screen once it boots with the daemons SpringBoard needs (about 15 minutes the first time, while
LaunchServices registers the runtime's apps): `SIM_DAEMONS=all`, or at least `cfprefsd`, `logd`, `notifyd`,
`runningboardd`, `lsd`, `containermanagerd`, `backboardd`, `SpringBoard`, `iokitsimd`, `installd`, `systemstatusd`,
`tccd` and `accountsd`, with `SIM_FRAMEBUFFER_FRAMEWORK` set for a screen. Give it a device type too:
`SIM_DEVICE_TYPE` is an Xcode `.simdevicetype` (`iPhone 16.simdevicetype`; Xcode keeps them in
`/Library/Developer/CoreSimulator/Profiles/DeviceTypes`). As CoreSimulator does, the device takes its model, its
screen and the profile's environment from it, and its capabilities tell MobileGestalt the screen's shape, without
which UIKit can't lay an app's window out. `SIM_MODEL_IDENTIFIER`, `SIM_SCREEN_*` and the rest override what the
device type says; `SIM_EXTRA_ENVIRONMENT` adds `NAME=VALUE` words to the environment of the device's processes.

`simboot.sh` also starts `simiokitd` beside the device: a Mac's simulator processes reach the host kernel's IOKit, and
Darling has none, so `simiokitd` serves `com.apple.iokitsimd` in the device (the runtime's IOKit looks it up when
`IOS_SIMULATOR_IOKITSIMD=1`) with the registry's root and `IOPMrootDomain`, which `backboardd` and SpringBoard open,
and `kIOReturnUnsupported` for the rest. Its log is the device's `Library/Logs/simiokitd.log`.

`siminstall.m` installs an app as `simctl install` does (installd, through MobileInstallation's `MIInstallerClient`;
LaunchServices refuses to install on iOS) and opens it as `simctl launch` does (SpringBoard). Build it for the
simulator against the runtime's frameworks (`-framework Foundation -framework MobileInstallation -framework
SpringBoardServices`) and sign it with `siminstall.entitlements`, e.g. with
[rcodesign](https://github.com/indygreg/apple-platform-rs) (`rcodesign sign --entitlements-xml-file
siminstall.entitlements siminstall`): SpringBoard opens apps only for clients entitled to. installd moves what it
installs into its staging directory, so install a copy:

```sh
cp -R Hello.app ~/Hello.app
/usr/libexec/ios-sim/simspawn /path/to/siminstall ~/Hello.app com.example.hello
/usr/libexec/ios-sim/simspawn /path/to/siminstall - com.example.hello   # open it again
```

The runtime has no `log` tool. `logd` writes its store when the device shuts down; `simlogarchive.sh` then collects
it, with the format strings `logd` saved for each binary, into a directory laid out as a `.logarchive`, which
[mandiant/macos-UnifiedLogs](https://github.com/mandiant/macos-UnifiedLogs)' `unifiedlog_iterator` reads on Linux:

```sh
/usr/libexec/ios-sim/simshutdown.sh
/usr/libexec/ios-sim/simlogarchive.sh ~/device.logarchive
# then, on Linux (the prefix's home is ~/.darling/Users/<login>), from a macos-UnifiedLogs checkout:
#   cargo run --release --manifest-path examples/unifiedlog_iterator/Cargo.toml -- \
#     -m log-archive -i ~/.darling/Users/$USER/device.logarchive -o device.jsonl
```

`launchd_sim`'s messages are simple dumps, which the reader shows with no process: it looks their sender up in the
dyld shared cache, which the runtime doesn't use here.

## What makes it work

- Darling's libSystem registers its extra callbacks with `libsystem_kernel` through `__libkernel_darling_init()`, so
  `_libkernel_functions` keeps Apple's layout, whatever version Apple's libSystem passes. A process whose libSystem
  did not register is running Apple's.
- Processes running Apple's libSystem see a supported host `kern.osrelease` (24.6.0, macOS 15.6), since the runtime's
  `libsystem_sim_kernel` traps on hosts older than it supports. Darling's own processes still see the emulated
  version. The host version is set at build time (`SIMULATOR_HOST_*` in xnu's emulation `CMakeLists.txt`); a runtime
  that requires a newer host needs it raised.
- The three host libraries export no duplicate symbols: Apple's libSystem re-exports `libsystem_sim_kernel` first, so
  a duplicate in `libsystem_kernel` would shadow the real implementation in `libsystem_platform`/`libsystem_pthread`.
- `libsystem_kernel`'s MIG stubs are built with `-novouchers`, as Apple does, so they do not call back into
  libdispatch while it initializes.
- SPIs the runtime imports from the host libraries (e.g. `mach_msg2_internal`, `mkfifoat`, `memcmp_zero`,
  `pthread_install_workgroup_functions_np`) are implemented or stubbed.
- Apple's libdispatch always runs in kevent-workqueue mode, and every serial queue is an XNU workloop. Darling's
  `libsystem_kernel` emulates both (`kevent_qos(KEVENT_FLAG_WORKQ)` and `kevent_id`) on libkqueue and its
  workqueue threads, following XNU's `kern_event.c`. Darling's own libdispatch uses neither, so
  `tests/src/workq_kevent.c` drives them directly, without libdispatch: configure with `-DENABLE_TESTS=ON`, build the
  `workq_kevent` target and run `darling shell /usr/libexec/darling-tests/workq_kevent` (it prints TAP-style lines).
- A booted device needs: fileports (`fileport_makeport`/`makefd`, held by darlingserver); `posix_spawn` port actions
  that set the bootstrap port, whose rights `launchd` makes immovable; `PSFA_FILEPORT_DUP2` file actions, which carry a
  job's stdio; local datagram sockets that fail with ENOBUFS when full, as XNU's do, rather than block (`launchd_sim`
  logs through one); `mach_vm_map()` of anonymous memory; and `__mac_syscall` answering the Sandbox checks as for an
  unsandboxed process (`sandbox_check()` allows; `sandbox_passthrough_access()` checks the file's permissions).
  Without the last, CFPreferences avoids `cfprefsd`, and `cfprefsd` refuses to write.
- Shutting a device down needs: EVFILT_SIGNAL knotes that see every signal delivered to the process, whichever thread
  takes it and whatever its disposition, as XNU's do (libdispatch's signal sources, which `launchd_sim` stops on;
  Darling's signal handlers post them; `signal_knotes.h` lists where they still differ from XNU's); a thread that has
  exited, such as the main thread after `dispatch_main()`, taking no more signals; `proc_terminate()`, which
  `launchd_sim` stops its daemons with; and a dead process's port rights dying with it, as XNU's do, so that `launchd`
  gets the device's Mach service back. `tests/src/kqueue_signal.c` checks the knotes (build the `kqueue_signal` target
  as `workq_kevent` above).
- `os_log` needs: Mach memory entries (`mach_make_memory_entry_64()`), which each process makes for its firehose buffer
  and sends to `logd`, which maps it with `mach_vm_map()`. Linux only shares memory that is a file, so darlingserver
  moves the range into a memfd when it makes the entry and maps the memfd for `mach_vm_map()`;
  `tests/src/memory_entry.c` checks them (build the `memory_entry` target as `workq_kevent` above). `logd` also needs
  the `kern.bootsessionuuid` sysctl; `csops()` returning the program's entitlements from its code signature
  (`logd_helper` serves only `logd`, which its entitlements show); and `proc_pidinfo()` reporting the program's
  `LC_UUID`, which `logd` files the program's format strings under.
- LaunchServices, installd and SpringBoard need the file system to answer as XNU's does: `getattrlist()`,
  `getattrlistbulk()` and `setattrlist()` with every attribute XNU knows (`tests/src/getattrlist.c`); `statfs()` naming
  the mount point as the process sees it, with the file's device as the file system ID, so CFURL finds a bundle's
  volume; `open()` with `O_SYMLINK` opening anything but a symlink normally (`tests/src/o_symlink.c`); extended
  attributes with XNU's names, kept in Linux's `user.` namespace (`tests/src/xattr.c`); `mincore()` on any address
  (`tests/src/mincore.c`; Apple's SQLite traps without it); and locks of an open file description, with a timeout
  (`F_OFD_SETLK`, `F_OFD_SETLKWTIMEOUT`; `tests/src/ofd_lock.c`): Apple's SQLite otherwise falls back to dot-file
  locks, which outlive a daemon that dies holding them.
- `thread_info()` and `thread_policy_get()` answer every flavor as XNU does (`tests/src/thread_info.c`); darlingserver
  used to abort on the ones it didn't know, taking every process down with it.
- FrontBoard trusts a client by its code signature: `csops()` gives a program's CDHash and entitlements from its own
  signature (`CS_OPS_CDHASH`, `CS_OPS_ENTITLEMENTS_BLOB`), so a tool that opens apps must be signed with the
  entitlements SpringBoard asks for (`siminstall.entitlements`).
- `kern.boottime` is Linux's boot time, the same in every call: libc's `CLOCK_MONOTONIC` is the time of day less it
  (`tests/src/clock_monotonic.c`).
- darlingserver provides the Mach voucher pieces libdispatch relies on: activity IDs, the voucher traps, and a BANK
  voucher attribute manager. The BANK manager is ported from XNU's `bank.c` without ledgers, thread groups or
  personas.
