cmake_minimum_required(VERSION 3.16)
project(fcitx5-google-cantonese-ime VERSION 1.4.2 LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)

# Default to /usr (not /usr/local) so a plain `cmake ..` builds an install tree
# that matches the Fcitx5 installation. Only overrides the default, untouched prefixes.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
endif()

# Native Fcitx5 CMake configuration. This provides the Fcitx5::Core and
# Fcitx5::Utils imported targets as well as the FCITX_INSTALL_* directory
# variables (addon dir, pkgdatadir, ...) used for the install rules below.
find_package(Fcitx5Core REQUIRED)
include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")
find_package(CURL REQUIRED)

# nlohmann/json for parsing Google Input Tools responses. Prefer a system package
# when available; otherwise fetch the release from GitHub at configure time.
# FetchContent requires git and network access during the cmake step.
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(fcitx5-google-ime MODULE
    src/engine.cc
    src/daemon_client.cc)

target_compile_features(fcitx5-google-ime PRIVATE cxx_std_17)
target_include_directories(fcitx5-google-ime PRIVATE src)
target_compile_options(fcitx5-google-ime PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion)

target_link_libraries(fcitx5-google-ime PRIVATE Fcitx5::Core Fcitx5::Utils)
if(TARGET CURL::libcurl)
    target_link_libraries(fcitx5-google-ime PRIVATE CURL::libcurl)
else()
    target_include_directories(fcitx5-google-ime PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(fcitx5-google-ime PRIVATE ${CURL_LIBRARIES})
endif()
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(fcitx5-google-ime PRIVATE nlohmann_json::nlohmann_json)
endif()

# When Fcitx5Core is found, enable the HAVE_FCITX5 define so engine.cc compiles.
if(TARGET Fcitx5::Core)
    target_compile_definitions(fcitx5-google-ime PRIVATE HAVE_FCITX5=1)
endif()

if(BUILD_TESTING)
    add_executable(config_migration_test tests/config_migration_test.cc)
    target_compile_features(config_migration_test PRIVATE cxx_std_17)
    target_include_directories(config_migration_test PRIVATE src)
    target_link_libraries(config_migration_test PRIVATE Fcitx5::Core Fcitx5::Utils)
    target_compile_options(config_migration_test PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion)
    add_test(NAME config_migration COMMAND config_migration_test)

    add_executable(daemon_client_test
        tests/daemon_client_test.cc
        src/daemon_client.cc)
    target_compile_features(daemon_client_test PRIVATE cxx_std_17)
    target_include_directories(daemon_client_test PRIVATE src)
    target_link_libraries(daemon_client_test PRIVATE Fcitx5::Utils)
    if(TARGET CURL::libcurl)
        target_link_libraries(daemon_client_test PRIVATE CURL::libcurl)
    else()
        target_include_directories(daemon_client_test PRIVATE ${CURL_INCLUDE_DIRS})
        target_link_libraries(daemon_client_test PRIVATE ${CURL_LIBRARIES})
    endif()
    if(TARGET nlohmann_json::nlohmann_json)
        target_link_libraries(daemon_client_test PRIVATE nlohmann_json::nlohmann_json)
    endif()
    target_compile_options(daemon_client_test PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion)
    add_test(NAME daemon_client COMMAND daemon_client_test)

    add_executable(candidate_paging_test tests/candidate_paging_test.cc)
    target_compile_features(candidate_paging_test PRIVATE cxx_std_17)
    target_include_directories(candidate_paging_test PRIVATE src)
    target_link_libraries(candidate_paging_test PRIVATE Fcitx5::Core Fcitx5::Utils)
    target_compile_options(candidate_paging_test PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion)
    add_test(NAME candidate_paging COMMAND candidate_paging_test)
endif()

# Generate addon/inputmethod metadata from .conf.in templates. configure_file
# substitutes @PROJECT_VERSION@ so the addon Version field stays in sync with the
# project version. The generated .conf files live in the build tree
# and are the only metadata installed, so the source tree ships templates only.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/data/addon/google-ime.conf.in
               ${CMAKE_CURRENT_BINARY_DIR}/data/addon/google-ime.conf @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/data/inputmethod/google-ime.conf.in
               ${CMAKE_CURRENT_BINARY_DIR}/data/inputmethod/google-ime.conf @ONLY)

# Install the addon shared library into the directory fcitx5 scans for addons
# (FCITX_INSTALL_ADDONDIR == ${CMAKE_INSTALL_FULL_LIBDIR}/fcitx5). The addon
# .conf references it as Library=libfcitx5-google-ime, which fcitx5 resolves
# against this directory via dlopen.
install(TARGETS fcitx5-google-ime LIBRARY DESTINATION ${FCITX_INSTALL_ADDONDIR})

# Install addon and inputmethod metadata so fcitx5 can discover the engine.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/data/addon/google-ime.conf
        DESTINATION ${FCITX_INSTALL_PKGDATADIR}/addon)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/data/inputmethod/google-ime.conf
        DESTINATION ${FCITX_INSTALL_PKGDATADIR}/inputmethod)
install(FILES data/icons/google-ime.svg
        DESTINATION ${FCITX_INSTALL_PKGDATADIR}/imicon)
install(FILES data/icons/google-ime.svg
        DESTINATION ${FCITX_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME ykshekGoogleIME.svg)
