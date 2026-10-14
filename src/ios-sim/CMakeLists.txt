project(ios-sim)

add_definitions(-nostdinc)

# Runs a program in a booted iOS Simulator device, like `simctl spawn` (see tools/ios-sim/README.md).
add_darling_executable(simspawn simspawn.c)

install(TARGETS simspawn DESTINATION libexec/darling/usr/libexec/ios-sim COMPONENT ios_sim)
install(PROGRAMS
	${CMAKE_SOURCE_DIR}/tools/ios-sim/simrun.sh
	${CMAKE_SOURCE_DIR}/tools/ios-sim/simboot.sh
	${CMAKE_SOURCE_DIR}/tools/ios-sim/simshutdown.sh
	DESTINATION libexec/darling/usr/libexec/ios-sim
	COMPONENT ios_sim
)
install(FILES ${CMAKE_SOURCE_DIR}/tools/ios-sim/simdevice.sh DESTINATION libexec/darling/usr/libexec/ios-sim COMPONENT ios_sim)
