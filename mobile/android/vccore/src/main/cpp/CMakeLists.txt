# libvccore.so: the shared C core plus its JNI bridge.
#
# The C sources live at the repository root and are compiled unchanged; the
# same files build vcd's tests (Makefile) and the iOS package (Package.swift).
cmake_minimum_required(VERSION 3.22.1)
project(vccore C)

# mobile/android/vccore/src/main/cpp -> repository root
get_filename_component(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

add_library(vccore SHARED
    ${CORE_DIR}/src/encoder.c
    ${CORE_DIR}/src/protocol.c
    ${CORE_DIR}/src/vc_client.c
    ${CORE_DIR}/src/layout.c
    ${CORE_DIR}/src/parser.c
    vccore_jni.c
)

set_target_properties(vccore PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
target_include_directories(vccore PRIVATE ${CORE_DIR}/include)
target_compile_options(vccore PRIVATE -O2 -Wall -Wextra -Werror)
# 16 KB page alignment, required for apps targeting Android 15+ devices.
target_link_options(vccore PRIVATE "-Wl,-z,max-page-size=16384")
target_link_libraries(vccore PRIVATE log m)
