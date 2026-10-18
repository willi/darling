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
