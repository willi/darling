project(darling-tests)

add_definitions(-nostdinc)

# Tests built with Darling's own toolchain and run inside the prefix, e.g. `darling shell /usr/libexec/darling-tests/workq_kevent`.
add_darling_executable(workq_kevent src/workq_kevent.c)
target_link_libraries(workq_kevent system)

add_darling_executable(kqueue_signal src/kqueue_signal.c)
target_link_libraries(kqueue_signal system)

install(TARGETS workq_kevent kqueue_signal DESTINATION libexec/darling/usr/libexec/darling-tests)
