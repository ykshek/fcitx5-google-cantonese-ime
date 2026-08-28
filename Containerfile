# Containerfile to build the fcitx5 plugin and RPM package across distros (Fedora base)
FROM quay.io/fedora/fedora:latest

LABEL description="Development and RPM build environment for fcitx5-google-cantonese-ime"
LABEL maintainer="Alex Shek <hms.starryfish@gmail.com>"

# Install build deps and RPM tooling
RUN dnf5 -y update && \
    dnf5 -y install \
        cmake \
        gcc-c++ \
        make \
        git \
        libcurl-devel \
        json-devel \
        pkgconf-pkg-config \
        fcitx5-devel \
        extra-cmake-modules \
        rpm-build \
        rpmdevtools \
        gawk \
    && dnf5 clean all

# Set up workspace directory
RUN mkdir -p /workspace
WORKDIR /workspace

# Set environment variables for development
ENV CMAKE_BUILD_TYPE=Release
ENV CMAKE_INSTALL_PREFIX=/usr
ENV QT_QPA_PLATFORM=offscreen

# Default to an interactive shell; use podman/docker run to execute build commands
CMD ["/bin/bash"]
