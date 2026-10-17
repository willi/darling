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

add_darling_executable(getattrlist src/getattrlist.c)
target_link_libraries(getattrlist system)

add_darling_executable(ulock_workloop src/ulock_workloop.c)
target_link_libraries(ulock_workloop system)

add_darling_executable(thread_info src/thread_info.c)
target_link_libraries(thread_info system)

add_darling_executable(mincore src/mincore.c)
target_link_libraries(mincore system)

add_darling_executable(o_symlink src/o_symlink.c)
target_link_libraries(o_symlink system)

add_darling_executable(xattr src/xattr.c)
target_link_libraries(xattr system)

add_darling_executable(ofd_lock src/ofd_lock.c)
target_link_libraries(ofd_lock system)

add_darling_executable(clock_monotonic src/clock_monotonic.c)
target_link_libraries(clock_monotonic system)

add_darling_executable(vm_protect src/vm_protect.c)
target_link_libraries(vm_protect system)

add_darling_executable(mk_timer_leeway src/mk_timer_leeway.c)
target_link_libraries(mk_timer_leeway system)

add_darling_executable(cond_timedwait src/cond_timedwait.c)
target_link_libraries(cond_timedwait system)

add_darling_executable(thread_suspend_exited src/thread_suspend_exited.c)
target_link_libraries(thread_suspend_exited system)

add_darling_executable(memory_entry src/memory_entry.c)
target_link_libraries(memory_entry system)

add_darling_executable(route_socket src/route_socket.c)
target_link_libraries(route_socket system)

add_darling_executable(madvise_unaligned src/madvise_unaligned.c)
target_link_libraries(madvise_unaligned system)

add_darling_executable(kqueue_sock src/kqueue_sock.c)
target_link_libraries(kqueue_sock system)

add_darling_executable(socket_recv src/socket_recv.c)
target_link_libraries(socket_recv system)

add_darling_executable(socket_send src/socket_send.c)
target_link_libraries(socket_send system)

add_darling_executable(statfs_flags src/statfs_flags.c)
target_link_libraries(statfs_flags system)

add_darling_executable(access_extended src/access_extended.c)
target_link_libraries(access_extended system)

add_darling_executable(proc_name src/proc_name.c)
target_link_libraries(proc_name system)

add_darling_executable(gethostuuid src/gethostuuid.c)
target_link_libraries(gethostuuid system)

install(TARGETS workq_kevent kqueue_signal kqueue_timer memory_entry timer_race getattrlistbulk getattrlist ulock_workloop thread_info mincore o_symlink xattr ofd_lock clock_monotonic vm_protect mk_timer_leeway cond_timedwait thread_suspend_exited route_socket madvise_unaligned kqueue_sock socket_recv socket_send statfs_flags access_extended proc_name gethostuuid DESTINATION libexec/darling/usr/libexec/darling-tests)
install(PROGRAMS run-all.sh DESTINATION libexec/darling/usr/libexec/darling-tests)
