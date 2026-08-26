# Containerfile to build the cpp plugin across distros (Fedora base)
FROM quay.io/fedora/fedora:latest

# Install build deps; adjust packages per distro as needed
RUN dnf -y update && \
    dnf -y install cmake gcc-c++ make libcurl-devel pkgconfig fcitx5-devel fcitx5-qt-devel gtk3-devel libX11-devel which python3 python3-pip podman && \
    dnf clean all

WORKDIR /work
COPY . /work

# Default to an interactive shell; use podman run to execute build commands
CMD ["/bin/bash"]
