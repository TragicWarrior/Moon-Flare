cmake_minimum_required(VERSION 3.14)
project(moon-flare
    VERSION 0.1.0
    DESCRIPTION "Moon Flare: multi-kind energy dashboard (daemon, REST, plugins, TUI)"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported. Run:\n"
        "  cmake -S . -B build && cmake --build build")
endif()

include(GNUInstallDirs)

# libviper/VDK drives the TUI (PR-7). Point at a sibling ../libviper by
# default; override with -DLIBVIPER_SOURCE_DIR=/path at configure time.
# moonflare is a --help stub until PR-7, so libvdk.so is not required yet.
# When the TUI lands, SYSTEM-include ${LIBVIPER_SOURCE_DIR}/vdk and
# ${LIBVIPER_SOURCE_DIR}/vkmio so #include <vdk.h> / <vkmio.h> match
# llama-chess and xd_bms_tui.c.
set(LIBVIPER_SOURCE_DIR "${PROJECT_SOURCE_DIR}/../libviper"
    CACHE PATH "Path to the libviper source tree (contains vdk/, vkmio/, libvdk.so)")
get_filename_component(LIBVIPER_SOURCE_DIR "${LIBVIPER_SOURCE_DIR}" ABSOLUTE)

set(MF_WARN -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
# protothread.h uses GCC labels-as-values; drop -Wpedantic there.
# -Wno-unused-parameter silences protothread_deinit when NDEBUG is set.
set(MF_WARN_PT -Wall -Wextra -Wshadow -Wconversion -Wno-unused-parameter)

# Vendored cJSON (Dave Gamble). Used by config (PR-3) and REST generate/parse.
add_library(mf_cjson STATIC third_party/cJSON/cJSON.c)
target_include_directories(mf_cjson PUBLIC ${CMAKE_SOURCE_DIR}/third_party/cJSON)
target_compile_options(mf_cjson PRIVATE -w)

# Vendored picohttpparser (Kazuho Oku et al., MIT).
add_library(mf_phr STATIC third_party/picohttpparser/picohttpparser.c)
target_include_directories(mf_phr PUBLIC ${CMAKE_SOURCE_DIR}/third_party/picohttpparser)
target_compile_options(mf_phr PRIVATE -w)

enable_testing()

add_executable(moonflared src/moonflared.c src/http/http_pt.c)
target_include_directories(moonflared PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/http)
target_compile_definitions(moonflared PRIVATE _GNU_SOURCE)
target_compile_options(moonflared PRIVATE ${MF_WARN_PT})
target_link_libraries(moonflared PRIVATE mf_phr)

add_executable(moonflare src/moonflare.c)
target_compile_options(moonflare PRIVATE ${MF_WARN})

add_executable(test_jk_proto
    src/jk/jk_proto.c
    tests/test_jk_proto.c
)
target_include_directories(test_jk_proto PRIVATE src/jk src)
add_test(NAME jk_proto COMMAND test_jk_proto)

add_executable(test_classic_codec
    src/classic/classic_codec.c
    tests/test_classic_codec.c
)
target_include_directories(test_classic_codec PRIVATE src/classic src)
add_test(NAME classic_codec COMMAND test_classic_codec)

add_executable(test_config
    src/config/config.c
    tests/test_config.c
)
target_include_directories(test_config PRIVATE src third_party/cJSON)
target_link_libraries(test_config PRIVATE mf_cjson)
add_test(NAME config COMMAND test_config)

add_executable(test_http_core tests/test_http_core.c)
target_compile_options(test_http_core PRIVATE ${MF_WARN})
add_test(NAME http_core COMMAND test_http_core $<TARGET_FILE:moonflared>)

add_test(NAME moonflared_help COMMAND moonflared --help)
add_test(NAME moonflare_help COMMAND moonflare --help)

install(TARGETS moonflared moonflare RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES contrib/moonflared.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/moon-flare)
install(FILES etc/moonflared.json.example etc/moonflare.json.example
        DESTINATION ${CMAKE_INSTALL_DATADIR}/moon-flare)
