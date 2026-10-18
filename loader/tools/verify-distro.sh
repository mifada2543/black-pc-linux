#!/usr/bin/env bash
# Verify the install instructions for one other distribution, in a container.
#
# What this does and does not close, stated up front because it decides whether the run is worth anything:
#
#   It DOES prove the package names in docs/INSTALL.md. A name that is wrong produces "no such package",
#   which is the failure a reader is most likely to hit, and a container is exactly the right tool for it:
#   installing packages and compiling is what containers are.
#
#   It does NOT prove the loader works. That needs the GPU, and a container reaches the GPU only through
#   /dev/dri plus host kernel modules -- where the 32-bit Mesa ICD is the fragile part. So a green run here
#   means "the instructions are not a lie", not "this port works on that distribution".
#
# Pick a DIFFERENT FAMILY, not another Ubuntu. Testing 26.04 against 24.04 proves nothing about Fedora.
#
# Usage:
#   tools/verify-distro.sh fedora   fedora:41
#   tools/verify-distro.sh arch     archlinux:latest
#   tools/verify-distro.sh suse     opensuse/tumbleweed
#
# Nothing is written outside docs/distro-verification/ and no image is kept.
set -u

cd "$(dirname "$0")/.." || exit 1

FAMILY="${1:-}"
IMAGE="${2:-}"
if [ -z "$FAMILY" ] || [ -z "$IMAGE" ]; then
    sed -n '3,20p' "$0" >&2
    exit 2
fi

case "$FAMILY" in
fedora) INSTALL="dnf install -y gcc-c++ glibc-devel.i686 libstdc++-devel.i686 vulkan-loader.i686 mesa-vulkan-drivers.i686 glslang-devel.i686 SDL2-devel.i686" ;;
arch)   INSTALL="pacman -Sy --noconfirm gcc shaderc lib32-shaderc lib32-sdl2 lib32-mesa vulkan-icd-loader lib32-vulkan-icd-loader" ;;
suse)   INSTALL="zypper -n install gcc-c++ glibc-devel vulkan-loader libvulkan_intel libvulkan_radeon glslang-devel libSDL2-devel" ;;
*)      printf 'unknown family "%s"; add it to the case above\n' "$FAMILY" >&2; exit 2 ;;
esac

OUTDIR="docs/distro-verification"
OUTFILE="$OUTDIR/$FAMILY.txt"
mkdir -p "$OUTDIR"

{
    printf 'BLACK loader -- install instructions verified by container\n'
    printf 'date        %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf 'family      %s\n' "$FAMILY"
    printf 'image       %s\n' "$IMAGE"
    printf 'scope       package names and the 32-bit toolchain ONLY.\n'
    printf '            It does not prove the loader draws: that needs the GPU on real hardware.\n'
    printf '\n$ %s\n' "$INSTALL"
} > "$OUTFILE"

printf 'pulling %s ...\n' "$IMAGE"
if ! docker pull "$IMAGE" >> "$OUTFILE" 2>&1; then
    printf 'the image could not be pulled; see %s\n' "$OUTFILE" >&2
    exit 1
fi

# No --device /dev/dri on purpose: that is what would prove the GPU path, and a container cannot prove it in
# a way that means anything for someone else's machine. The claims under test are all about packages.
printf 'running the install and the check ...\n'
docker run --rm -v "$(pwd):/repo:ro" -w /repo "$IMAGE" bash -lc "
    set -u
    printf '### /etc/os-release\n'
    cat /etc/os-release
    printf '\n### install\n'
    $INSTALL 2>&1
    printf '\n### check-env.sh\n'
    # The repo is mounted read-only, so check-env.sh runs against the host's build/ by mistake unless told
    # otherwise. It only reads those paths to decide whether to print a note, so the answer it gives about
    # "the build" is this container's, which is what we want to record.
    bash loader/tools/check-env.sh 2>&1
    printf '\n### exit code\n%s\n' \"\$?\"
" >> "$OUTFILE" 2>&1
RC=$?

{
    printf '\n### verdict\n'
    if [ "$RC" = "0" ]; then
        printf 'the container ran to completion; read the check-env.sh section above.\n'
        printf 'A green summary means the package names are right for %s.\n' "$FAMILY"
    else
        printf 'the container exited %s; the failure is in the output above.\n' "$RC"
        printf 'That is exactly what a container is for: docs/INSTALL.md is now known to be wrong for\n'
        printf '%s, which is better than a reader finding that out.\n' "$FAMILY"
    fi
    printf '\nThis is NOT the A11 gate on its own. The gate asks for instructions verified on two\n'
    printf 'distributions; this contributes one distribution and says nothing about whether the loader\n'
    printf 'runs on the hardware it will actually meet.\n'
} >> "$OUTFILE"

printf '\nwrote %s\n' "$OUTFILE"
docker rmi "$IMAGE" > /dev/null 2>&1
exit "$RC"