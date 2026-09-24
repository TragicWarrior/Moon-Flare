cmake_minimum_required(VERSION 3.14)
project(moon-flare
    VERSION 0.6.0
    DESCRIPTION "Moon Flare: multi-kind energy dashboard (daemon, REST, plugins, TUI)"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# One version for the daemon, CLI, MCP and TUI identity strings.
add_compile_definitions(MF_VERSION="${PROJECT_VERSION}")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported. Run:\n"
        "  cmake -S . -B build && cmake --build build")
endif()

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSESW REQUIRED IMPORTED_TARGET ncursesw)
pkg_check_modules(LIBSYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

# libviper/VDK drives the TUI (PR-7). Point at a sibling ../libviper by
# default; override with -DLIBVIPER_SOURCE_DIR=/path at configure time.
# TUI consumes a sibling libviper checkout (same as xd-battery).
set(LIBVIPER_SOURCE_DIR "${PROJECT_SOURCE_DIR}/../libviper"
    CACHE PATH "Path to the libviper source tree (contains vdk/, vkmio/, libvdk.so)")
get_filename_component(LIBVIPER_SOURCE_DIR "${LIBVIPER_SOURCE_DIR}" ABSOLUTE)
if(NOT EXISTS "${LIBVIPER_SOURCE_DIR}/libvdk.so")
    message(FATAL_ERROR
        "libvdk.so not found at ${LIBVIPER_SOURCE_DIR}/libvdk.so. "
        "Build libviper first, or pass -DLIBVIPER_SOURCE_DIR=<path>.")
endif()

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

add_executable(moonflared src/moonflared.c src/http/http_pt.c src/plugin/loader.c src/config/config.c src/rest/routes.c src/rest/discover.c src/device/device.c src/system/system.c)
target_include_directories(moonflared PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/http
    ${CMAKE_SOURCE_DIR}/src/plugin
    ${CMAKE_SOURCE_DIR}/src/config
    ${CMAKE_SOURCE_DIR}/src/device
    ${CMAKE_SOURCE_DIR}/src/rest)
target_compile_definitions(moonflared PRIVATE _GNU_SOURCE)
target_compile_options(moonflared PRIVATE ${MF_WARN_PT})
target_link_libraries(moonflared PRIVATE mf_phr mf_cjson ${CMAKE_DL_LIBS})

add_executable(moonflare
    src/moonflare.c
    src/debug/mf_backtrace.c
    src/tui/ui_screen.c
    src/tui/menubar.c
    src/tui/dashboard.c
    src/tui/pack_view.c
    src/tui/device_settings.c
    src/tui/http_client.c
    src/tui/layout.c
    src/tui/mouse.c
    src/tui/picker.c
    src/system/system.c
    src/config/config.c)
target_include_directories(moonflare PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/tui
    ${CMAKE_SOURCE_DIR}/src/config
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(moonflare SYSTEM PRIVATE
    ${LIBVIPER_SOURCE_DIR}/vdk ${LIBVIPER_SOURCE_DIR}/vkmio)
target_link_libraries(moonflare PRIVATE
    mf_cjson "${LIBVIPER_SOURCE_DIR}/libvdk.so" PkgConfig::NCURSESW m)
target_compile_definitions(moonflare PRIVATE _GNU_SOURCE)
target_compile_options(moonflare PRIVATE ${MF_WARN})
set_target_properties(moonflare PROPERTIES
    OUTPUT_NAME moonflare-tui
    BUILD_RPATH ${LIBVIPER_SOURCE_DIR}
    ENABLE_EXPORTS ON)

add_executable(moonflare-cli
    src/cli/main.c
    src/cli/cli_http.c
    src/cli/cli_addr.c
    src/cli/cli_render.c
    src/cli/mcp.c
    src/config/config.c)
target_include_directories(moonflare-cli PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/config
    ${CMAKE_SOURCE_DIR}/third_party/cJSON)
target_compile_definitions(moonflare-cli PRIVATE _GNU_SOURCE)
target_compile_options(moonflare-cli PRIVATE ${MF_WARN})
target_link_libraries(moonflare-cli PRIVATE mf_cjson m)

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
target_compile_definitions(test_config PRIVATE MF_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME config COMMAND test_config)

add_library(mf_demo SHARED src/plugins/demo/plugin.c)
target_include_directories(mf_demo PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(mf_demo PROPERTIES
    PREFIX "lib" OUTPUT_NAME "mf_demo"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/demo_plugins)
target_compile_options(mf_demo PRIVATE ${MF_WARN})
target_link_libraries(mf_demo PRIVATE m)

add_executable(test_demo_plugin tests/test_demo_plugin.c src/plugins/demo/plugin.c)
target_include_directories(test_demo_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_demo_plugin PRIVATE m)
add_test(NAME demo_plugin COMMAND test_demo_plugin)

add_library(mf_charger_classic SHARED
    src/plugins/charger_classic/plugin.c
    src/plugins/charger_classic/probe.c
    src/classic/classic_codec.c)
target_include_directories(mf_charger_classic PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/classic)
set_target_properties(mf_charger_classic PROPERTIES
    PREFIX "lib" OUTPUT_NAME "mf_charger_classic"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/classic_plugins)
target_compile_definitions(mf_charger_classic PRIVATE _GNU_SOURCE)
target_compile_options(mf_charger_classic PRIVATE ${MF_WARN_PT})

add_executable(test_classic_plugin tests/test_classic_plugin.c)
target_include_directories(test_classic_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_classic_plugin PRIVATE ${CMAKE_DL_LIBS})
set_target_properties(test_classic_plugin PROPERTIES
    BUILD_RPATH ${CMAKE_BINARY_DIR}/classic_plugins)
add_test(NAME classic_plugin COMMAND test_classic_plugin
    $<TARGET_FILE:fake_classic> $<TARGET_FILE:mf_charger_classic>)

add_library(mf_battery_xd SHARED
    src/plugins/battery_xd/plugin.c
    src/xd/bms_proto.c
    src/usb/usb_id.c)
target_include_directories(mf_battery_xd PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/xd
    ${CMAKE_SOURCE_DIR}/src/usb)
set_target_properties(mf_battery_xd PROPERTIES
    PREFIX "lib" OUTPUT_NAME "mf_battery_xd"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/xd_plugins)
target_compile_definitions(mf_battery_xd PRIVATE _GNU_SOURCE)
target_compile_options(mf_battery_xd PRIVATE ${MF_WARN_PT})

add_executable(test_xd_proto src/xd/bms_proto.c tests/test_xd_proto.c)
target_include_directories(test_xd_proto PRIVATE ${CMAKE_SOURCE_DIR}/src/xd)
target_compile_definitions(test_xd_proto PRIVATE _GNU_SOURCE)
target_compile_options(test_xd_proto PRIVATE ${MF_WARN_PT})
add_test(NAME xd_proto COMMAND test_xd_proto)

add_executable(test_xd_plugin tests/test_xd_plugin.c src/xd/bms_proto.c)
target_include_directories(test_xd_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/xd)
target_link_libraries(test_xd_plugin PRIVATE ${CMAKE_DL_LIBS} util)
target_compile_definitions(test_xd_plugin PRIVATE _GNU_SOURCE)
target_compile_options(test_xd_plugin PRIVATE ${MF_WARN_PT})
set_target_properties(test_xd_plugin PROPERTIES
    BUILD_RPATH ${CMAKE_BINARY_DIR}/xd_plugins)
add_test(NAME xd_plugin COMMAND test_xd_plugin $<TARGET_FILE:mf_battery_xd>)

# weather.gov service plugin: libcurl (multi interface) + cJSON.  Built only
# when libcurl's development files are present (libcurl4-openssl-dev).
find_package(CURL)
if(CURL_FOUND)
    set_target_properties(mf_cjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
    find_package(ZLIB REQUIRED)
    include(GNUInstallDirs)
    add_library(mf_service_weathergov SHARED
        src/plugins/service_weathergov/plugin.c
        src/plugins/service_weathergov/zcta.c)
    target_include_directories(mf_service_weathergov PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party/cJSON)
    target_link_libraries(mf_service_weathergov PRIVATE
        CURL::libcurl ZLIB::ZLIB mf_cjson m)
    target_compile_definitions(mf_service_weathergov PRIVATE
        MF_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/moon-flare")

    # ZIP table: parsing, unzip and lookup, without the network.
    add_executable(test_zcta tests/test_zcta.c
        src/plugins/service_weathergov/zcta.c)
    target_include_directories(test_zcta PRIVATE
        ${CMAKE_SOURCE_DIR}/src/plugins/service_weathergov)
    target_link_libraries(test_zcta PRIVATE ZLIB::ZLIB)
    target_compile_definitions(test_zcta PRIVATE
        MF_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME zcta COMMAND test_zcta)
    set_target_properties(mf_service_weathergov PROPERTIES
        PREFIX "lib" OUTPUT_NAME "mf_service_weathergov"
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/weather_plugins)
    target_compile_definitions(mf_service_weathergov PRIVATE _GNU_SOURCE)
    target_compile_options(mf_service_weathergov PRIVATE ${MF_WARN})
else()
    message(STATUS "libcurl not found: skipping the weather.gov plugin")
endif()

add_library(mf_battery_jk SHARED
    src/plugins/battery_jk/plugin.c
    src/jk/jk_proto.c)
target_include_directories(mf_battery_jk PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/jk)
set_target_properties(mf_battery_jk PROPERTIES
    PREFIX "lib" OUTPUT_NAME "mf_battery_jk"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/jk_plugins)
target_compile_definitions(mf_battery_jk PRIVATE _GNU_SOURCE)
target_compile_options(mf_battery_jk PRIVATE ${MF_WARN_PT})

add_executable(fake_gatt tests/fake_gatt.c src/jk/jk_proto.c)
target_include_directories(fake_gatt PRIVATE ${CMAKE_SOURCE_DIR}/src/jk)
target_compile_definitions(fake_gatt PRIVATE _GNU_SOURCE)
target_compile_options(fake_gatt PRIVATE ${MF_WARN_PT})

add_executable(test_jk_plugin tests/test_jk_plugin.c)
target_include_directories(test_jk_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_jk_plugin PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(test_jk_plugin PRIVATE _GNU_SOURCE)
target_compile_options(test_jk_plugin PRIVATE ${MF_WARN_PT})
set_target_properties(test_jk_plugin PROPERTIES
    BUILD_RPATH ${CMAKE_BINARY_DIR}/jk_plugins)
add_test(NAME jk_plugin COMMAND test_jk_plugin
    $<TARGET_FILE:fake_gatt> $<TARGET_FILE:mf_battery_jk>)

add_executable(test_usb_id tests/test_usb_id.c src/usb/usb_id.c)
target_include_directories(test_usb_id PRIVATE ${CMAKE_SOURCE_DIR}/src/usb)
target_compile_definitions(test_usb_id PRIVATE _GNU_SOURCE)
target_compile_options(test_usb_id PRIVATE ${MF_WARN_PT})
add_test(NAME usb_id COMMAND test_usb_id)

add_executable(test_classic_discover tests/test_classic_discover.c)
target_include_directories(test_classic_discover PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_classic_discover PRIVATE ${CMAKE_DL_LIBS})
set_target_properties(test_classic_discover PROPERTIES
    BUILD_RPATH ${CMAKE_BINARY_DIR}/classic_plugins)
add_test(NAME classic_discover COMMAND test_classic_discover
    $<TARGET_FILE:fake_classic> $<TARGET_FILE:mf_charger_classic>)

add_library(mf_stub SHARED tests/stub_plugin.c)
target_include_directories(mf_stub PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(mf_stub PROPERTIES
    PREFIX "lib" OUTPUT_NAME "mf_stub"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/stub_plugins)
target_compile_options(mf_stub PRIVATE ${MF_WARN})

set(MF_SQLITE_INC ${CMAKE_SOURCE_DIR}/third_party/sqlite/include)
set(MF_SQLITE_LIB ${CMAKE_SOURCE_DIR}/third_party/sqlite/lib/libsqlite3.so.0.8.6)

# Compile the history subsystem into the daemon and link SQLite so the
# poll loop can capture samples.
target_sources(moonflared PRIVATE ${CMAKE_SOURCE_DIR}/src/history/history.c)
target_include_directories(moonflared PRIVATE
    ${CMAKE_SOURCE_DIR}/src/history ${MF_SQLITE_INC})
target_link_libraries(moonflared PRIVATE ${MF_SQLITE_LIB})
set_target_properties(moonflared PROPERTIES BUILD_RPATH
    ${CMAKE_SOURCE_DIR}/third_party/sqlite/lib)

add_executable(test_history tests/test_history.c src/history/history.c)
target_include_directories(test_history PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/history ${MF_SQLITE_INC} third_party/cJSON)
target_link_libraries(test_history PRIVATE ${MF_SQLITE_LIB} mf_cjson)
set_target_properties(test_history PROPERTIES BUILD_RPATH
    ${CMAKE_SOURCE_DIR}/third_party/sqlite/lib)
add_test(NAME history COMMAND test_history)

add_executable(test_routes tests/test_routes.c src/rest/routes.c src/rest/discover.c src/device/device.c src/plugin/loader.c src/config/config.c src/history/history.c src/system/system.c)
target_include_directories(test_routes PRIVATE
    ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/rest
    ${CMAKE_SOURCE_DIR}/src/device ${CMAKE_SOURCE_DIR}/src/plugin
    ${CMAKE_SOURCE_DIR}/src/config ${CMAKE_SOURCE_DIR}/src/history
    ${CMAKE_SOURCE_DIR}/include ${MF_SQLITE_INC})
target_link_libraries(test_routes PRIVATE mf_cjson ${CMAKE_DL_LIBS} ${MF_SQLITE_LIB})
add_test(NAME routes COMMAND test_routes)

add_executable(test_http_devices tests/test_http_devices.c)
target_compile_options(test_http_devices PRIVATE ${MF_WARN})
add_test(NAME http_devices COMMAND test_http_devices
    $<TARGET_FILE:moonflared> $<TARGET_FILE_DIR:mf_demo>)

add_executable(test_loader tests/test_loader.c src/plugin/loader.c)
target_include_directories(test_loader PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/plugin)
target_link_libraries(test_loader PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME loader COMMAND test_loader $<TARGET_FILE_DIR:mf_stub>)
# The same stub built like a pre-0.3.0 plugin (short ops entries).
add_library(mf_stub_old SHARED tests/stub_plugin.c)
target_include_directories(mf_stub_old PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(mf_stub_old PRIVATE MF_STUB_OLD_ABI)
set_target_properties(mf_stub_old PROPERTIES
    PREFIX "lib" OUTPUT_NAME "mf_stub"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/stub_old_plugins)
add_test(NAME loader_old_abi
         COMMAND test_loader $<TARGET_FILE_DIR:mf_stub_old> old)

add_executable(fake_classic tests/fake_classic.c)
target_compile_options(fake_classic PRIVATE ${MF_WARN})

add_executable(test_fake_classic tests/test_fake_classic.c src/classic/classic_codec.c)
target_include_directories(test_fake_classic PRIVATE src/classic src)
add_test(NAME fake_classic COMMAND test_fake_classic $<TARGET_FILE:fake_classic>)

add_executable(test_http_core tests/test_http_core.c)
target_compile_options(test_http_core PRIVATE ${MF_WARN})
add_test(NAME http_core COMMAND test_http_core $<TARGET_FILE:moonflared>)

add_executable(mf_gatt src/gatt/mf_gatt.c)
target_compile_definitions(mf_gatt PRIVATE _GNU_SOURCE)
target_compile_options(mf_gatt PRIVATE ${MF_WARN})
target_link_libraries(mf_gatt PRIVATE PkgConfig::LIBSYSTEMD)
add_test(NAME mf_gatt_help COMMAND mf_gatt --help)

add_test(NAME moonflared_help COMMAND moonflared --help)
add_test(NAME moonflare_help COMMAND moonflare --help)

add_executable(test_tui_layout tests/test_tui_layout.c src/tui/layout.c)
target_include_directories(test_tui_layout PRIVATE ${CMAKE_SOURCE_DIR}/src/tui)
target_compile_options(test_tui_layout PRIVATE ${MF_WARN})
add_test(NAME tui_layout COMMAND test_tui_layout)

add_executable(test_system tests/test_system.c src/system/system.c)
target_include_directories(test_system PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_system PRIVATE ${MF_WARN})
target_link_libraries(test_system PRIVATE m)
add_test(NAME system COMMAND test_system)

add_executable(test_tui_http tests/test_tui_http.c src/tui/http_client.c)
target_include_directories(test_tui_http PRIVATE ${CMAKE_SOURCE_DIR}/src/tui)
target_compile_definitions(test_tui_http PRIVATE _GNU_SOURCE)
target_compile_options(test_tui_http PRIVATE ${MF_WARN})
add_test(NAME tui_http COMMAND test_tui_http)

add_test(NAME moonflare_dump COMMAND moonflare --dump-layout)
add_test(NAME moonflare_dump_pack COMMAND moonflare --dump-layout pack)
add_test(NAME moonflare_dump_charger COMMAND moonflare --dump-layout charger)

install(TARGETS moonflared moonflare RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS mf_gatt RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
# Match KD 20 / example plugin_dir: $prefix/lib/moon-flare, not multiarch LIBDIR.
install(TARGETS mf_charger_classic mf_battery_xd mf_battery_jk
        LIBRARY DESTINATION lib/moon-flare)
if(TARGET mf_service_weathergov)
    install(TARGETS mf_service_weathergov LIBRARY DESTINATION lib/moon-flare)
    install(FILES data/zcta.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/moon-flare)
endif()
# The demo plugin fakes a battery and a charger for development.  Keep it
# out of production plugin dirs unless asked for.
option(MF_INSTALL_DEMO "Install the demo plugin (libmf_demo.so)" OFF)
if(MF_INSTALL_DEMO)
    install(TARGETS mf_demo LIBRARY DESTINATION lib/moon-flare)
endif()
install(FILES contrib/moonflared.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/moon-flare)
install(FILES etc/moonflared.json.example etc/moonflare.json.example
        DESTINATION ${CMAKE_INSTALL_DATADIR}/moon-flare)
