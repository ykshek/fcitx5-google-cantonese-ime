cmake_minimum_required(VERSION 3.10)
project(fcitx5-google-ime LANGUAGES CXX)

# Use pkg-config to find fcitx5 development files. On Fedora these are provided
# by packages like 'fcitx5-devel' or similar; on Debian/Ubuntu install 'libfcitx5-dev'.
find_package(PkgConfig REQUIRED)

# Try common pkg-config module names; project may adjust as needed.
set(FCITX5_PKG_NAMES fcitx5 fcitx5-1)

foreach(_name IN LISTS FCITX5_PKG_NAMES)
    if (NOT DEFINED FCITX5_FOUND)
        pkg_check_modules(FCITX5_QUIET QUIET ${_name})
        if (FCITX5_QUIET_FOUND)
            set(FCITX5_PKG ${_name})
            set(FCITX5_FOUND TRUE)
            pkg_check_modules(FCITX5 REQUIRED ${_name})
        endif()
    endif()
endforeach()

if(NOT FCITX5_FOUND)
    message(WARNING "Could not find fcitx5 pkg-config module. The build will still produce a skeleton binary, but full fcitx5 integration requires fcitx5 dev headers.")
endif()

add_library(fcitx5-google-ime SHARED src/fcitx5_google_ime.cc)

target_compile_features(fcitx5-google-ime PRIVATE cxx_std_17)

target_include_directories(fcitx5-google-ime PRIVATE src)

if(FCITX5_FOUND)
    target_include_directories(fcitx5-google-ime PRIVATE ${FCITX5_INCLUDE_DIRS})
    target_link_libraries(fcitx5-google-ime PRIVATE ${FCITX5_LIBRARIES} pthread curl)
    target_compile_definitions(fcitx5-google-ime PRIVATE HAVE_FCITX5=1)
else()
    # Link at least libcurl so the example daemon-caller can build.
    find_package(CURL REQUIRED)
    target_link_libraries(fcitx5-google-ime PRIVATE ${CURL_LIBRARIES})
endif()

# Installation hints (adjust prefix as needed):
install(TARGETS fcitx5-google-ime LIBRARY DESTINATION lib/fcitx5/modules)
