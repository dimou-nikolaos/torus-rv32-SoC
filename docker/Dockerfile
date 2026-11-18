# Torus RV32 SoC tool image. Built only by CI from tools/versions.env;
# developers pull it from the registry (see SPEC.md, Toolchain and environment).
#   docker build -f docker/Dockerfile --build-arg BASE_IMAGE=... .
ARG BASE_IMAGE

# OpenROAD (Bazel build) and the ORFS flow; only the installed result is copied.
FROM ${BASE_IMAGE} AS openroad
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git libc6-dev libxml2 libtinfo6 zlib1g libstdc++6 python3 \
    && rm -rf /var/lib/apt/lists/*
COPY tools/versions.env tools/build_openroad.sh /tmp/tools/
RUN /tmp/tools/build_openroad.sh /tmp/tools/versions.env /opt/torus && rm -rf /root/.cache

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
COPY --from=openroad /opt/torus/openroad /opt/torus/openroad
COPY --from=openroad /opt/torus/orfs /opt/torus/orfs

ENV TORUS_TOOLS=/opt/torus \
    PDK_ROOT=/opt/torus/pdk \
    PDK=sky130A \
    ORFS_FLOW=/opt/torus/orfs/flow \
    PATH=/opt/torus/venv/bin:/opt/torus/riscv-gcc/bin:/opt/torus/oss-cad-suite/bin:/opt/torus/openroad/bin:${PATH}
COPY tools/versions.env /opt/torus/versions.env
WORKDIR /work
