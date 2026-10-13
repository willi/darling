project(ios-sim)

add_definitions(-nostdinc)

# Runs a program in a booted iOS Simulator device, like `simctl spawn` (see tools/ios-sim/README.md).
add_darling_executable(simspawn simspawn.c)

install(TARGETS simspawn DESTINATION libexec/darling/usr/libexec/ios-sim)
