cmake_minimum_required(VERSION 3.10)
project(fcitx5-google-ime LANGUAGES CXX)

# Find native Fcitx5 CMake configuration
find_package(Fcitx5Core REQUIRED)
find_package(CURL REQUIRED)

add_library(fcitx5-google-ime SHARED src/fcitx5_google_ime.cc)

target_compile_features(fcitx5-google-ime PRIVATE cxx_std_17)
target_include_directories(fcitx5-google-ime PRIVATE src)

# Link imported targets directly
target_link_libraries(fcitx5-google-ime PRIVATE Fcitx5::Core ${CURL_LIBRARIES})

# Installation target
install(TARGETS fcitx5-google-ime LIBRARY DESTINATION ${FCITX_INSTALL_PKGLIBDIR}/modules)
