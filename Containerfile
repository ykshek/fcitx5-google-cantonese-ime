# Containerfile to build the cpp plugin across distros (Fedora base)
FROM quay.io/fedora/fedora:latest

# Install build deps; adjust packages per distro as needed
RUN dnf5 -y update && \
    dnf5 -y install \
        cmake \
        gcc-c++ \
        make \
        libcurl-devel \
        pkgconfig \
        fcitx5-devel \
        fcitx5-qt-devel \
        extra-cmake-modules \
    && dnf5 clean all


# Set up workspace directory
RUN mkdir -p cpp/build
WORKDIR /work

# Set environment variables for development
ENV CMAKE_BUILD_TYPE=Release
ENV CMAKE_INSTALL_PREFIX=/usr/local
ENV QT_QPA_PLATFORM=offscreen

# Default to an interactive shell; use podman run to execute build commands
CMD ["/bin/bash"]
