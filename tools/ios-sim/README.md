# Running an iOS Simulator runtime under Darling

Apple's iOS Simulator runtimes (`iOS <version>.simruntime`) are x86_64 Mach-O user space that runs on the macOS host
kernel through `dyld_sim`. Under Darling they run unmodified: Darling's `libsystem_kernel`, `libsystem_platform` and
`libsystem_pthread` play the host libraries that the runtime's `libsystem_sim_*` shims re-export.

Status: command-line tools from the iOS 26.5 runtime (e.g. `usr/bin/tab2space`) run to completion. Foundation and
UIKit processes have not been tried yet.

## Usage

1. Extract a runtime (e.g. from Xcode's `iOS_<version>_Simulator_Runtime.dmg`). Do not modify its files: its code
   signatures should keep verifying.
2. Inside `darling shell`:

   ```sh
   SIM_RUNTIME_ROOT="/Volumes/SystemRoot/<path>/iOS 26.5.simruntime/Contents/Resources/RuntimeRoot" \
     tools/ios-sim/simrun.sh usr/bin/tab2space < input.txt
   ```

`simrun.sh` sets the environment CoreSimulator gives simulator processes (`SIMULATOR_*`, `HOME`, `TMPDIR`, ...).
Device data defaults to `~/Library/Developer/CoreSimulator/Devices/<udid>/data`.

## What makes it work

- `_libkernel_functions`: Apple's libSystem passes version 5, whose new fields sit where Darling appends its own
  fields after version 4. Darling reads its fields only from a table passed by Darling's libSystem.
- Processes running Apple's libSystem see a supported host `kern.osrelease` (24.6.0, macOS 15.6), since the runtime's
  `libsystem_sim_kernel` traps on hosts older than it supports. Darling's own processes still see the emulated
  version.
- The three host libraries export no duplicate symbols: Apple's libSystem re-exports `libsystem_sim_kernel` first, so
  a duplicate in `libsystem_kernel` would shadow the real implementation in `libsystem_platform`/`libsystem_pthread`.
- `libsystem_kernel`'s MIG stubs are built with `-novouchers`, as Apple does, so they do not call back into
  libdispatch while it initializes.
- SPIs the runtime imports from the host libraries (e.g. `mach_msg2_internal`, `mkfifoat`, `memcmp_zero`,
  `pthread_install_workgroup_functions_np`) are implemented or stubbed.
