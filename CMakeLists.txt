cmake_minimum_required(VERSION 3.0)
project(ptt)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(LIBEVDEV REQUIRED libevdev)

add_executable(ptt ptt.c)
target_link_libraries(ptt ${PIPEWIRE_LIBRARIES} ${LIBEVDEV_LIBRARIES})
target_include_directories(ptt PUBLIC ${PIPEWIRE_INCLUDE_DIRS} ${LIBEVDEV_INCLUDE_DIRS})
target_compile_options(ptt PUBLIC ${PIPEWIRE_CFLAGS_OTHER} ${LIBEVDEV_CFLAGS_OTHER})
