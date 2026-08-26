cmake_minimum_required(VERSION 3.10)
project(fcitx5-google-ime LANGUAGES CXX)

# Find native Fcitx5 CMake configuration
find_package(Fcitx5Core REQUIRED)
find_package(CURL REQUIRED)

add_library(fcitx5-google-ime SHARED src/daemon_client.cc src/engine.cc)

target_compile_features(fcitx5-google-ime PRIVATE cxx_std_17)
target_include_directories(fcitx5-google-ime PRIVATE src)

# When Fcitx5Core is found, enable the HAVE_FCITX5 define so engine.cc compiles
if(TARGET Fcitx5::Core)
    target_compile_definitions(fcitx5-google-ime PRIVATE HAVE_FCITX5=1)
endif()

# Link imported targets directly
target_link_libraries(fcitx5-google-ime PRIVATE Fcitx5::Core ${CURL_LIBRARIES})

# Ensure the Fcitx5Core include directories are available to the target.
# Some CMake package configs export INTERFACE include directories via the
# imported target; use generator expression to propagate them explicitly.
if(TARGET Fcitx5::Core)
    target_include_directories(fcitx5-google-ime PRIVATE $<TARGET_PROPERTY:Fcitx5::Core,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Installation target
install(TARGETS fcitx5-google-ime LIBRARY DESTINATION ${FCITX_INSTALL_PKGLIBDIR}/modules)

# Install addon and inputmethod metadata so fcitx5 can discover the engine.
# Use the install directories provided by the Fcitx5Core package (FCITX_INSTALL_DATADIR)
# which map to the appropriate share/fcitx5 path for the system.
install(FILES ${CMAKE_SOURCE_DIR}/data/addon/google-ime.conf DESTINATION ${FCITX_INSTALL_DATADIR}/addon)
install(FILES ${CMAKE_SOURCE_DIR}/data/inputmethod/google-ime.conf DESTINATION ${FCITX_INSTALL_DATADIR}/inputmethod)

# Optional: install the daemon scripts into datadir for developer convenience
install(FILES ${CMAKE_SOURCE_DIR}/../daemon/google_tools.py ${CMAKE_SOURCE_DIR}/../daemon/server.py DESTINATION ${FCITX_INSTALL_DATADIR}/google-ime OPTIONAL)
