# Torus RV32 SoC tool image. Built only by CI from tools/versions.env;
# developers pull it from the registry (see SPEC.md, Toolchain and environment).
#   docker build -f docker/Dockerfile --build-arg BASE_IMAGE=... .
ARG BASE_IMAGE
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git make g++ gcc ccache cmake ninja-build \
        python3 python3-venv python3-dev \
        device-tree-compiler libboost-regex-dev libboost-system-dev \
        zlib1g-dev liblz4-dev perl \
    && rm -rf /var/lib/apt/lists/*

COPY tools/versions.env tools/install_pinned.sh /tmp/tools/
RUN /tmp/tools/install_pinned.sh /tmp/tools/versions.env /opt/torus && rm -rf /tmp/tools

ENV TORUS_TOOLS=/opt/torus \
    PDK_ROOT=/opt/torus/pdk \
    PDK=sky130A \
    PATH=/opt/torus/venv/bin:/opt/torus/riscv-gcc/bin:/opt/torus/oss-cad-suite/bin:${PATH}
COPY tools/versions.env /opt/torus/versions.env
WORKDIR /work
