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
- UIKit processes have not been tried yet.

## Usage

1. Build Darling with the `ios_sim` component (`-DCOMPONENTS=stock,ios_sim`, or `all`). It installs `simrun.sh`,
   `simboot.sh`, `simshutdown.sh` and `simspawn` in the prefix's `/usr/libexec/ios-sim`.
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
`launchd_sim` logs to `/private/var/tmp/com.apple.CoreSimulator.SimDevice.<udid>/launchd.log`.
`simshutdown.sh` unloads the device's job, as `simctl shutdown` does: `launchd` sends `launchd_sim` SIGTERM, and
`launchd_sim` stops the device's processes and exits. The device's variables (`SIM_UDID` and so on) are in
`simdevice.sh`.

The runtime has no `log` tool. `logd` writes its store when the device shuts down; `simlogarchive.sh` then collects
it, with the format strings `logd` saved for each binary, into a directory laid out as a `.logarchive`, which
[mandiant/macos-UnifiedLogs](https://github.com/mandiant/macos-UnifiedLogs)' `unifiedlog_iterator` reads on Linux:

```sh
/usr/libexec/ios-sim/simshutdown.sh
/usr/libexec/ios-sim/simlogarchive.sh "/Volumes/SystemRoot$HOME/device.logarchive"
# then, on Linux, from a macos-UnifiedLogs checkout:
#   cargo run --release --manifest-path examples/unifiedlog_iterator/Cargo.toml -- \
#     -m log-archive -i ~/device.logarchive -o device.jsonl
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
- darlingserver provides the Mach voucher pieces libdispatch relies on: activity IDs, the voucher traps, and a BANK
  voucher attribute manager. The BANK manager is ported from XNU's `bank.c` without ledgers, thread groups or
  personas.
