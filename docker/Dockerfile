# Palm OS m68k build environment: prc-tools-remix (GCC m68k-palmos), PilRC,
# and the Palm OS SDKs from github.com/jichu4n/palm-os-sdk.
# The upstream packages are amd64-only; on Apple Silicon this runs under emulation.
FROM --platform=linux/amd64 debian:bookworm-slim

ARG PRC_TOOLS_VER=2.3.6
ARG PILRC_TAG=2.3.91
ARG PILRC_VER=3.2.91
ARG PALM_SDK=sdk-3.5

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl unzip make python3 xxd \
 && curl -fsSLo /tmp/prc.deb https://github.com/jichu4n/prc-tools-remix/releases/download/v${PRC_TOOLS_VER}/prc-tools-remix_${PRC_TOOLS_VER}.bookworm_amd64.deb \
 && curl -fsSLo /tmp/pilrc.deb https://github.com/jichu4n/pilrc/releases/download/${PILRC_TAG}/pilrc_${PILRC_VER}.bookworm_amd64.deb \
 && apt-get install -y --no-install-recommends /tmp/prc.deb /tmp/pilrc.deb \
 && curl -fsSLo /tmp/sdk.zip https://github.com/jichu4n/palm-os-sdk/archive/master.zip \
 && unzip -q /tmp/sdk.zip -d /tmp \
 && mv /tmp/palm-os-sdk-master /opt/palmdev \
 && palmdev-prep -d ${PALM_SDK} \
 && rm -rf /tmp/*.deb /tmp/sdk.zip /var/lib/apt/lists/*

WORKDIR /src
