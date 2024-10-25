cmake_minimum_required(VERSION 3.1)

project(pixelpilot VERSION 1.1.0)

set(CMAKE_CXX_STANDARD 17)

add_compile_options("-Wno-address-of-packed-member")

set(SOURCE_FILES 
        src/drm.h
        src/drm.c
        src/osd.h
        src/osd.c
        src/dvr.h
        src/dvr.cpp
        src/rtp.h
        src/rtp.c
        src/mavlink.h
        src/mavlink.c
        src/main.cpp
        src/main.h
        src/scheduling_helper.hpp
        src/gstrtpreceiver.cpp
        src/gstrtpreceiver.h
        src/gstdvrreceiver.cpp
        src/gstdvrreceiver.h
        src/gpio.cpp
        src/gpio.h)

include_directories("/usr/include/libdrm" "/usr/include/cairo")

# Find the gpiod library
find_library(GPIOD_LIB gpiod REQUIRED)

# Include directories for gpiod
include_directories(/usr/include)

configure_file("${PROJECT_NAME}_config.h.in" "${PROJECT_NAME}_config.h")

add_executable(${PROJECT_NAME} ${SOURCE_FILES})

# Link libraries
target_link_libraries(${PROJECT_NAME} rockchip_mpp pthread drm m cairo ${GPIOD_LIB})

# Embed gstreamer
find_package(PkgConfig REQUIRED)
pkg_search_module(GST REQUIRED
        gstreamer-1.0>=1.4
        gstreamer-app-1.0>=1.4
)
pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)
target_link_libraries(${PROJECT_NAME} PkgConfig::gstreamer PkgConfig::gstreamer-app)

if(CMAKE_BUILD_TYPE MATCHES "Debug")
  set(
    CMAKE_C_FLAGS
    "${CMAKE_C_FLAGS} -Werror -fsanitize=undefined -fsanitize=address"
  )
  target_link_options(${PROJECT_NAME}
    BEFORE PUBLIC -fsanitize=undefined PUBLIC -fsanitize=address
  )
endif()

target_include_directories(${PROJECT_NAME} PUBLIC "${PROJECT_BINARY_DIR}")

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
