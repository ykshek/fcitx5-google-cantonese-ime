cmake_minimum_required(VERSION 3.10)
project(fcitx5-google-ime LANGUAGES CXX)

# This CMake file is a minimal skeleton. To build the actual fcitx5 plugin
# install fcitx5 development headers and libraries, then set FCITX5_DIR or
# use pkg-config to find the components. Adjust as needed for your distro.

find_package(PkgConfig REQUIRED)
pkg_check_modules(FCITX5 REQUIRED fcitx5)

add_library(fcitx5-google-ime SHARED src/fcitx5_google_ime.cc)

target_include_directories(fcitx5-google-ime PRIVATE ${FCITX5_INCLUDE_DIRS})
target_link_libraries(fcitx5-google-ime PRIVATE ${FCITX5_LIBRARIES} pthread curl)

target_compile_features(fcitx5-google-ime PRIVATE cxx_std_17)

# Installation hints (adjust prefix as needed):
install(TARGETS fcitx5-google-ime LIBRARY DESTINATION lib/fcitx5/modules)
