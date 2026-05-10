ARG UBUNTU_TAG=22.04
FROM docker.io/ubuntu:${UBUNTU_TAG}

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Switch to Australian mirror before apt-get update
RUN sed -i 's|http://archive.ubuntu.com/ubuntu/|http://au.archive.ubuntu.com/ubuntu/|g; \
            s|http://security.ubuntu.com/ubuntu/|http://au.archive.ubuntu.com/ubuntu/|g' \
    /etc/apt/sources.list

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
    # ----- Core libraries----- \
    libboost-all-dev \
    libeigen3-dev \
    openssl \
    libssl-dev

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
