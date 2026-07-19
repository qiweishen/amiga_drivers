ARG UBUNTU_TAG=22.04
FROM docker.io/ubuntu:${UBUNTU_TAG}

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# ============================================================
# Base Development Tools
# ============================================================
RUN apt-get update && apt-get install -y --no-install-recommends \
    # ----- Build essentials ----- \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
	# ----- Version Control ----- \
    git \
    # ----- Debugging & Analysis ----- \
    gdb \
    lldb \
    valgrind \
    # ----- Code Formatting ----- \
    clang-format \
	# ----- Utilities ----- \
    wget \
    curl \
    ca-certificates \
    sudo \
    locales \
    # ----- 3rd_party libraries ----- \
    libboost-all-dev \
    libeigen3-dev \
    openssl \
    libssl-dev \
    qtbase5-dev \
    libqt5serialport5-dev

# ============================================================
# Wayland tools (needed by Ubuntu 26.04)
# ============================================================
RUN apt-get install -y --no-install-recommends \
    libwayland-client0 libwayland-cursor0 libwayland-egl1 libxkbcommon0

# ============================================================
# eBUS SDK for JAI
# ============================================================
COPY gox_driver/resource/eBUS_SDK_JAI_Ubuntu-22.04-x86_64-*.deb /tmp/ebus/
RUN apt-get update && \
    (dpkg -i /tmp/ebus/eBUS_SDK_JAI_*.deb || apt-get install -f -y) && \
    rm -rf /tmp/ebus
ENV EBUS_SDK_ROOT=/opt/jai/ebus_sdk/Ubuntu-22.04-x86_64
ENV GENICAM_ROOT_V3_4=${EBUS_SDK_ROOT}/lib/genicam
ENV LD_LIBRARY_PATH=${EBUS_SDK_ROOT}/lib:${EBUS_SDK_ROOT}/lib/genicam/bin/Linux64_x64

# ============================================================
# Clean cache to reduce image size
# ============================================================
RUN apt-get clean && rm -rf /var/lib/apt/lists/*

# ============================================================
# Locale Configuration
# ============================================================
RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8
ENV LC_ALL=en_US.UTF-8

# ============================================================
# Create Non-root User
# ============================================================
ARG USERNAME=dev
ARG USER_UID=1000
ARG USER_GID=${USER_UID}
RUN groupadd --gid ${USER_GID} ${USERNAME} && \
    useradd --uid ${USER_UID} --gid ${USER_GID} -m -s /bin/bash ${USERNAME} && \
    # Grant passwordless sudo \
    echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers.d/${USERNAME} && \
    chmod 0440 /etc/sudoers.d/${USERNAME}

# ============================================================
# Workspace Setup
# ============================================================
WORKDIR /workspace
RUN chown ${USERNAME}:${USERNAME} /workspace

USER ${USERNAME}
