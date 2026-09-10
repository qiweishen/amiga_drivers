ARG UBUNTU_TAG=22.04
FROM docker.io/ubuntu:${UBUNTU_TAG}

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# =============================================================
# Base development tools
# =============================================================
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
    zlib1g-dev \
    openssl \
    libssl-dev \
    qtbase5-dev \
    libqt5serialport5-dev \
    libcli11-dev \
    libgtest-dev \
    libgmock-dev

# =============================================================
# eBUS SDK dependencies
# =============================================================
RUN apt-get install -y --no-install-recommends \
    libqt5opengl5 libavcodec58

# =============================================================
# Wayland client libraries (GUI forwarding defaults to X11/XWayland)
# =============================================================
RUN apt-get install -y --no-install-recommends \
    libwayland-client0 libwayland-cursor0 libwayland-egl1 libxkbcommon0

# =============================================================
# eBUS SDK (single Pleora 6.5.1 edition for BOTH the JAI Go-X and the Specim FX10)
# =============================================================
COPY resource/eBUS_SDK_Ubuntu-22.04-x86_64-*.deb /tmp/ebus/
RUN dpkg -i /tmp/ebus/eBUS_SDK_Ubuntu-22.04-x86_64-*.deb
ENV EBUS_SDK_ROOT=/opt/pleora/ebus_sdk/Ubuntu-22.04-x86_64
# Fx10's FindeBUS reads PUREGEV_ROOT; Go-X's FindeBUS reads EBUS_SDK_ROOT
ENV PUREGEV_ROOT=${EBUS_SDK_ROOT}
ENV GENICAM_ROOT_V3_4=${EBUS_SDK_ROOT}/lib/genicam
ENV LD_LIBRARY_PATH=${EBUS_SDK_ROOT}/lib:${EBUS_SDK_ROOT}/lib/genicam/bin/Linux64_x64

# =============================================================
# Clean cache to reduce image size
# =============================================================
RUN apt-get clean && rm -rf /var/lib/apt/lists/* && rm -rf /tmp/ebus

# =============================================================
# Locale configuration
# =============================================================
RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8
ENV LC_ALL=en_US.UTF-8

# =============================================================
# Create non-root User
# =============================================================
ARG USERNAME=dev
ARG USER_UID=1000
ARG USER_GID=${USER_UID}
RUN groupadd --gid ${USER_GID} ${USERNAME} && \
    useradd --uid ${USER_UID} --gid ${USER_GID} -m -s /bin/bash ${USERNAME} && \
    # Grant passwordless sudo \
    echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers.d/${USERNAME} && \
    chmod 0440 /etc/sudoers.d/${USERNAME}

# =============================================================
# Workspace setup
# =============================================================
WORKDIR /workspace
RUN chown ${USERNAME}:${USERNAME} /workspace

USER ${USERNAME}
