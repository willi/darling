project(darling-tests)

add_definitions(-nostdinc)

# Tests built with Darling's own toolchain and run inside the prefix, e.g. `darling shell /usr/libexec/darling-tests/workq_kevent`.
add_darling_executable(workq_kevent src/workq_kevent.c)
target_link_libraries(workq_kevent system)

add_darling_executable(kqueue_signal src/kqueue_signal.c)
target_link_libraries(kqueue_signal system)

add_darling_executable(kqueue_timer src/kqueue_timer.c)
target_link_libraries(kqueue_timer system)

add_darling_executable(timer_race src/timer_race.c)
target_link_libraries(timer_race system)

add_darling_executable(getattrlistbulk src/getattrlistbulk.c)
target_link_libraries(getattrlistbulk system)

add_darling_executable(memory_entry src/memory_entry.c)
target_link_libraries(memory_entry system)

install(TARGETS workq_kevent kqueue_signal kqueue_timer memory_entry timer_race getattrlistbulk DESTINATION libexec/darling/usr/libexec/darling-tests)
