ARG UBUNTU_TAG=22.04
ARG UV_VERSION=0.11.15
FROM ghcr.io/astral-sh/uv:${UV_VERSION} AS uv
FROM docker.io/ubuntu:${UBUNTU_TAG} AS python-runtime

# Shared Python runtime for the GUI and the controller inside the driver image
ENV DEBIAN_FRONTEND=noninteractive \
    UV_PYTHON_INSTALL_DIR=/opt/python \
    UV_PROJECT_ENVIRONMENT=/opt/venv \
    UV_LINK_MODE=copy \
    UV_NO_CACHE=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    AMIGA_PYTHON=/opt/venv/bin/python \
    PATH="/opt/venv/bin:${PATH}"

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libglib2.0-0 libgl1 libgomp1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=uv /uv /usr/local/bin/uv
ARG PYTHON_VERSION=3.12.13
WORKDIR /opt/amiga-python
COPY pyproject.toml uv.lock ./
RUN uv sync --locked --no-dev --no-install-project --python ${PYTHON_VERSION}

FROM python-runtime AS gui
ARG USER_UID=1000
ARG USER_GID=${USER_UID}
ENV HOME=/tmp NICEGUI_STORAGE_PATH=/tmp/nicegui
WORKDIR /workspace
USER ${USER_UID}:${USER_GID}
CMD ["/opt/venv/bin/python", "-B", "-m", "app.main"]

FROM python-runtime AS drivers

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
    procps \
    iproute2 \
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

# The API keeps the development container alive
CMD ["/opt/venv/bin/python", "-B", "-m", "app.controller"]
