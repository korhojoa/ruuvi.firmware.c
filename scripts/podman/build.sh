#!/usr/bin/env bash
# Build the firmware in the ruuvi-fw-build container (rootless podman).
#
#   scripts/podman/build.sh                          # make ruuvitag_b, all variants
#   scripts/podman/build.sh ruuvitag_b VARIANTS=default
#   scripts/podman/build.sh -- <a shell command in /work/src>
#
# The script builds the image at the first use. The output goes to
# src/targets/<board>/armgcc/, the same as a build on the host.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMAGE="localhost/ruuvi-fw-build"
SDK="nRF5_SDK_15.3.0_59ac345"

if ! podman image exists "${IMAGE}"; then
    podman build -t "${IMAGE}" "${ROOT}/scripts/podman"
fi

# The SDK symbolic link at the repository root, the same as the CI workflow
# makes (git ignores it). The link target is only in the container, thus the
# test is for the link itself.
if [ ! -L "${ROOT}/${SDK}" ] && [ ! -e "${ROOT}/${SDK}" ]; then
    ln -s "/opt/${SDK}" "${ROOT}/${SDK}"
fi

if [ "${1:-}" = "--" ]; then
    shift
    CMD=("$@")
else
    CMD=(make ruuvitag_b "$@")
fi

TTY=()
if [ -t 0 ]; then TTY=(-it); fi

exec podman run --rm "${TTY[@]}" --userns=keep-id \
    -v "${ROOT}:/work:Z" -w /work/src \
    "${IMAGE}" "${CMD[@]}"
