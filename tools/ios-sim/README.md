# Running an iOS Simulator runtime under Darling

Apple's iOS Simulator runtimes (`iOS <version>.simruntime`) are x86_64 Mach-O user space that runs on the macOS host
kernel through `dyld_sim`. Under Darling they run unmodified: Darling's `libsystem_kernel`, `libsystem_platform` and
`libsystem_pthread` play the host libraries that the runtime's `libsystem_sim_*` shims re-export.

Status, with the iOS 26.5 runtime:
- command-line tools such as `usr/bin/tab2space` run to completion;
- Foundation/XPC clients such as `usr/bin/defaults read` run to completion;
- a device boots: Apple's `launchd_sim` runs the runtime's daemons and the programs `simspawn` asks for. `defaults
  write` and `defaults read` go through the device's own `cfprefsd`, which `launchd_sim` starts on demand;
- UIKit processes have not been tried yet.

## Usage

1. Build Darling with the `ios_sim` component (`-DCOMPONENTS=stock,ios_sim`, or `all`). It installs `simrun.sh`,
   `simboot.sh` and `simspawn` in the prefix's `/usr/libexec/ios-sim`.
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
```

`simboot.sh` boots the device as CoreSimulator does: a job in Darling's `launchd` runs the runtime's
`launchd_sim_trampoline`, which hands the device's bootstrap port to `launchd_sim`. The device loads the daemons in
`SIM_DAEMONS` (only `cfprefsd` by default). `simspawn` passes `SIMCTL_CHILD_*` variables to the program, as
`simctl spawn` does; `SIMCTL_CHILD_OS_ACTIVITY_DT_MODE=1` prints its `os_log` messages on stderr.
`launchd_sim` logs to `/private/var/tmp/com.apple.CoreSimulator.SimDevice.<udid>/launchd.log`.

Not working yet:
- `logd`: `os_log`'s buffers need `mach_make_memory_entry()` memory entries, which darlingserver doesn't have (with
  `logd` loaded, darlingserver aborts in that stub);
- shutting a device down: `launchd_sim` doesn't act on `launchd`'s SIGTERM, and `launchctl unload` of its job hangs.

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
- darlingserver provides the Mach voucher pieces libdispatch relies on: activity IDs, the voucher traps, and a BANK
  voucher attribute manager. The BANK manager is ported from XNU's `bank.c` without ledgers, thread groups or
  personas.
