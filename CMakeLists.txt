set(CMAKE_INCLUDE_CURRENT_DIR ON)

cmake_minimum_required(VERSION 4.0)
project(waydroid-better-sensors)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

pkg_check_modules(GIO REQUIRED gio-2.0)
pkg_check_modules(GIO_UNIX REQUIRED gio-unix-2.0)
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(GOBJECT REQUIRED gobject-2.0)
pkg_check_modules(GLIB_UTIL REQUIRED libglibutil)
pkg_check_modules(GBINDER REQUIRED libgbinder>=1.1.20)
pkg_check_modules(QMI REQUIRED qmi-glib)

find_path(SSC_INCLUDE_DIR
    NAMES ssc_glib.h ssc.h
    PATH_SUFFIXES libssc
    PATHS /usr/include /usr/local/include
)

find_library(SSC_LIBRARY
    NAMES ssc
    PATHS /usr/lib /usr/local/lib /usr/lib64 /usr/local/lib64
)

if(NOT SSC_INCLUDE_DIR)
    set(SSC_INCLUDE_DIR "/usr/include/libssc")
endif()

if(NOT SSC_LIBRARY)
    set(SSC_LIBRARY ssc)
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -pthread")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -pthread")



add_executable(
    waydroid-sensord

    service.cpp
)

target_link_libraries(waydroid-sensord PUBLIC
    ${GIO_LDFLAGS} ${GIO_LIBRARIES}
    ${GIO_UNIX_LDFLAGS} ${GIO_UNIX_LIBRARIES}
    ${GLIB_LDFLAGS} ${GLIB_LIBRARIES}
    ${GOBJECT_LDFLAGS} ${GOBJECT_LIBRARIES}
    ${GLIB_UTIL_LDFLAGS} ${GLIB_UTIL_LIBRARIES}
    ${GBINDER_LDFLAGS} ${GBINDER_LIBRARIES}
    ${QMI_LDFLAGS} ${QMI_LIBRARIES}
    ${SSC_LIBRARY}
)

target_include_directories(waydroid-sensord PUBLIC
    ${GIO_INCLUDE_DIRS}
    ${GIO_UNIX_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIRS}
    ${GOBJECT_INCLUDE_DIRS}
    ${GLIB_UTIL_INCLUDE_DIRS}
    ${GBINDER_INCLUDE_DIRS}
    ${QMI_INCLUDE_DIRS}
    ${SSC_INCLUDE_DIR}
)

install(TARGETS waydroid-sensord RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
