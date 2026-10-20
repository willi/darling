project(ios-sim)

add_definitions(-nostdinc)

# Runs a program in a booted iOS Simulator device, like `simctl spawn` (see tools/ios-sim/README.md).
add_darling_executable(simspawn simspawn.c)

# Runs a program as another user: simboot.sh starts the device's launchd_sim as SIM_UID through it.
add_darling_executable(simasuid simasuid.c)

# The IOKit a booted device sees (see simiokitd.c): the iokit subsystem's server, as Apple's iokitsimd is built.
set(MIG_EXTRA_FLAGS "-DIOKIT=1 -DIOKITSIMD=1")
set(MIG_NO_XTRACE TRUE)
mig(iokit.defs)
unset(MIG_NO_XTRACE)
unset(MIG_EXTRA_FLAGS)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
add_darling_executable(simiokitd simiokitd.c simiokitd_unsupported.c ${CMAKE_CURRENT_BINARY_DIR}/iokitServer.c)
# device_types.h declares the io_* types only for IOKit itself
target_compile_definitions(simiokitd PRIVATE IOKIT=1 IOKITSIMD=1)

install(TARGETS simspawn simasuid simiokitd DESTINATION libexec/darling/usr/libexec/ios-sim COMPONENT ios_sim)
install(PROGRAMS
	${CMAKE_SOURCE_DIR}/tools/ios-sim/simrun.sh
	${CMAKE_SOURCE_DIR}/tools/ios-sim/simboot.sh
	${CMAKE_SOURCE_DIR}/tools/ios-sim/simshutdown.sh
	${CMAKE_SOURCE_DIR}/tools/ios-sim/simlogarchive.sh
	DESTINATION libexec/darling/usr/libexec/ios-sim
	COMPONENT ios_sim
)
install(FILES ${CMAKE_SOURCE_DIR}/tools/ios-sim/simdevice.sh DESTINATION libexec/darling/usr/libexec/ios-sim COMPONENT ios_sim)

# The bootstrap simboot.sh writes, against a Mac's (tools/ios-sim/test/bootstrap-golden.sh, README.md): a key missing
# or wrong there fails no boot, only jobs inside the device, out of sight, so it fails the build instead. It runs
# simboot.sh with launchctl stubbed, in about a second, and again only when the scripts or the golden master change.
set(ios_sim_test ${CMAKE_SOURCE_DIR}/tools/ios-sim/test)
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bootstrap-golden.stamp
	COMMAND sh ${ios_sim_test}/bootstrap-golden.sh
	COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/bootstrap-golden.stamp
	DEPENDS
		${CMAKE_SOURCE_DIR}/tools/ios-sim/simboot.sh
		${CMAKE_SOURCE_DIR}/tools/ios-sim/simdevice.sh
		${ios_sim_test}/bootstrap-golden.sh
		${ios_sim_test}/bootstrap.golden
		${ios_sim_test}/bootstrap.allow
		${ios_sim_test}/plist2lines.awk
	COMMENT "simboot.sh's bootstrap against a Mac's"
	VERBATIM
)
add_custom_target(ios_sim_bootstrap_golden ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bootstrap-golden.stamp)

# simboot.sh stops at once, saying why, when a job file came out with an empty value or launchd refused a job
# (tools/ios-sim/test/simboot-fail-fast.sh): such a boot otherwise waits out its whole timeout. A few seconds, with
# launchctl and sed stubbed, again only when simboot.sh or the test changes.
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/simboot-fail-fast.stamp
	COMMAND sh ${ios_sim_test}/simboot-fail-fast.sh
	COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/simboot-fail-fast.stamp
	DEPENDS
		${CMAKE_SOURCE_DIR}/tools/ios-sim/simboot.sh
		${CMAKE_SOURCE_DIR}/tools/ios-sim/simdevice.sh
		${ios_sim_test}/simboot-fail-fast.sh
	COMMENT "simboot.sh stops at a failed job write and at a refused job"
	VERBATIM
)
add_custom_target(ios_sim_simboot_fail_fast ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/simboot-fail-fast.stamp)
