cmake_minimum_required(VERSION 3.14)
project(fcitx5-google-ime LANGUAGES CXX)

# Find native Fcitx5 CMake configuration
find_package(Fcitx5Core REQUIRED)
include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")
find_package(CURL REQUIRED)

# nlohmann/json for parsing Google Input Tools responses. Prefer a system
# package (e.g. Fedora's json-devel / Debian's nlohmann-json3-dev) when
# available; otherwise fetch the single-header release from GitHub at configure
# time. FetchContent requires git and network access during the cmake step.
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

add_library(fcitx5-google-ime SHARED src/daemon_client.cc src/engine.cc)

target_compile_features(fcitx5-google-ime PRIVATE cxx_std_17)
target_include_directories(fcitx5-google-ime PRIVATE src ${Fcitx5Core_INCLUDE_DIRS} ${FCITX_INSTALL_INCLUDEDIR}/fcitx5)

# When Fcitx5Core is found, enable the HAVE_FCITX5 define so engine.cc compiles
if(TARGET Fcitx5::Core)
    target_compile_definitions(fcitx5-google-ime PRIVATE HAVE_FCITX5=1)
endif()

# Link imported targets directly. nlohmann_json is header-only; both the system
# package and the FetchContent path provide the nlohmann_json::nlohmann_json
# target, so link it unconditionally.
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

# Installation target
install(TARGETS fcitx5-google-ime LIBRARY DESTINATION ${FCITX_INSTALL_PKGLIBDIR}/modules)

# Install addon and inputmethod metadata so fcitx5 can discover the engine.
# Use the install directories provided by the Fcitx5Core package (FCITX_INSTALL_DATADIR)
# which map to the appropriate share/fcitx5 path for the system.
install(FILES ${CMAKE_SOURCE_DIR}/data/addon/google-ime.conf DESTINATION ${FCITX_INSTALL_DATADIR}/addon)
install(FILES ${CMAKE_SOURCE_DIR}/data/inputmethod/google-ime.conf DESTINATION ${FCITX_INSTALL_DATADIR}/inputmethod)
