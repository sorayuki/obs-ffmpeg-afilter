cmake_minimum_required(VERSION 3.16)
project(obs-ffmpeg-afilter VERSION 0.2.0.0)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if (CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(PLAT "32bit")
    set(LINK_PLAT "x86")
    set(DEP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/x86)
elseif (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(PLAT "64bit")
    set(LINK_PLAT "x64")
    set(DEP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/x64)
else ()
    message(FATAL_ERROR "?")
endif ()

if (${CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT})
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/obs-bin" CACHE PATH "" FORCE)
endif()

# build obs lib
execute_process(COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/pexports.exe"
    "${CMAKE_CURRENT_SOURCE_DIR}/obs-bin-${LINK_PLAT}/bin/${PLAT}/obs.dll"
    OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/libobs.def"
    COMMAND_ECHO STDOUT
)
execute_process(COMMAND ${CMAKE_LINKER}
    "/lib" "/MACHINE:${LINK_PLAT}"
    "/def:${CMAKE_CURRENT_BINARY_DIR}/libobs.def"
    "/out:${CMAKE_CURRENT_BINARY_DIR}/libobs.lib")

configure_file(version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version.rc)

add_library(obs-ffmpeg-afilter SHARED
    obs-ffmpeg-afilter.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc
)
target_include_directories(obs-ffmpeg-afilter PRIVATE
    obs-src/libobs
    ${DEP_DIR}/include
)
target_link_directories(obs-ffmpeg-afilter PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${DEP_DIR}/lib
)
target_link_libraries(obs-ffmpeg-afilter PRIVATE
    libobs
    avutil
    avfilter
)
target_link_options(obs-ffmpeg-afilter PRIVATE
    -SAFESEH:NO
)

install(TARGETS obs-ffmpeg-afilter RUNTIME
    DESTINATION obs-plugins/${PLAT}
)

