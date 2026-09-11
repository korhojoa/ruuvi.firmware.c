# Build image for ruuvi.firmware.c. It has the same tools as
# .github/workflows/build_fw.yaml: gcc-arm-none-eabi 7-2018-q2, the Nordic
# SDK 15.3.0_59ac345, nrfutil, intelhex and mergehex (from the nRF Command
# Line Tools 10.24.2).
#
# Build:  podman build -t ruuvi-fw-build scripts/podman
# Use:    scripts/podman/build.sh [make args]     (refer to that script)
FROM docker.io/library/ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates wget unzip bzip2 make git python3 python3-pip \
        libusb-1.0-0 libncurses5 \
    && rm -rf /var/lib/apt/lists/*

# The Arm GNU toolchain goes to the path that Makefile.posix of the nRF5 SDK
# uses. The CI workflow makes a symbolic link to the same path.
RUN wget -q -O /tmp/gcc.tar.bz2 \
        "https://developer.arm.com/-/media/Files/downloads/gnu-rm/7-2018q2/gcc-arm-none-eabi-7-2018-q2-update-linux.tar.bz2" \
    && tar -xjf /tmp/gcc.tar.bz2 -C /usr/local \
    && rm /tmp/gcc.tar.bz2 \
    && /usr/local/gcc-arm-none-eabi-7-2018-q2-update/bin/arm-none-eabi-gcc --version | head -1

# The nRF Command Line Tools give mergehex (and nrfjprog, which is not used).
# The CI workflow gets this file from storage.ruuvi.com. That name did not
# resolve in DNS when this image was made, thus the Nordic URL is used.
# The version is the same.
RUN wget -q -O /tmp/nrf-clt.deb \
        "https://nsscprodmedia.blob.core.windows.net/prod/software-and-other-downloads/desktop-software/nrf-command-line-tools/sw/versions-10-x-x/10-24-2/nrf-command-line-tools_10.24.2_amd64.deb" \
    && (dpkg -i /tmp/nrf-clt.deb || true) \
    && rm -f /tmp/nrf-clt.deb \
    && mergehex --version

# nrfutil (the legacy python package, for DFU packages) and intelhex.
RUN pip3 install --no-cache-dir intelhex nrfutil && nrfutil version

# The Nordic SDK 15.3.0, the same archive that the CI workflow uses. The
# Nordic URL is used because the CI mirror on storage.ruuvi.com did not
# resolve in DNS.
RUN wget -q -O /tmp/nRF5_SDK_15.3.0_59ac345.zip \
        https://developer.nordicsemi.com/nRF5_SDK/nRF5_SDK_v15.x.x/nRF5_SDK_15.3.0_59ac345.zip \
    && unzip -q /tmp/nRF5_SDK_15.3.0_59ac345.zip -d /opt \
    && rm -f /tmp/nRF5_SDK_15.3.0_59ac345.zip \
    && test -d /opt/nRF5_SDK_15.3.0_59ac345/modules

# The repository is bind-mounted at /work. The Makefiles refer to the SDK as
# ../../../../nRF5_SDK_15.3.0_59ac345 from src/targets/<board>/armgcc, that
# is /work/nRF5_SDK_15.3.0_59ac345. The script build.sh makes that symbolic
# link (git ignores it).
RUN git config --system --add safe.directory '*'
WORKDIR /work/src
