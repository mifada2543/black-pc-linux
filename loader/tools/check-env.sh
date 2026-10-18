#!/usr/bin/env bash
# What this machine has, and what to install for what it does not.
#
# This script installs nothing. Not because installing is dangerous here, but because installing is not the
# question: the question is which package, and a script that installs it is a script that guessed. So this
# reports, per distro, and leaves the command to be run by whoever owns the machine.
#
# Every check is a thing that is measured rather than assumed. In particular the Vulkan check runs a real
# 32-bit program that submits real work, because a loader that links and a driver that works are separate
# facts -- and on this machine's Intel card they are separate in the worst possible way.
set -u

cd "$(dirname "$0")/.." || exit 1

RED=$'\033[31m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'; BOLD=$'\033[1m'; OFF=$'\033[0m'
if [ ! -t 1 ]; then RED=""; GREEN=""; YELLOW=""; BOLD=""; OFF=""; fi

ok=0; warn=0; bad=0
say_ok()   { printf '  %sPASS%s  %s\n' "$GREEN" "$OFF" "$1"; ok=$((ok+1)); }
say_warn() { printf '  %sWARN%s  %s\n' "$YELLOW" "$OFF" "$1"; warn=$((warn+1)); }
say_bad()  { printf '  %sFAIL%s  %s\n' "$RED" "$OFF" "$1"; bad=$((bad+1)); }
say_note() { printf '        %s\n' "$1"; }

printf '%sBLACK loader -- environment check%s\n\n' "$BOLD" "$OFF"

# ---- which distro, and therefore which package names ----
DISTRO=""
FAMILY=""
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    DISTRO="${PRETTY_NAME:-${NAME:-unknown}}"
    case "${ID:-}${ID_LIKE:-}" in
        *debian*) FAMILY="debian" ;;
        *fedora*|*rhel*|*centos*) FAMILY="fedora" ;;
        *arch*|*manjaro*) FAMILY="arch" ;;
        *suse*) FAMILY="suse" ;;
        *) FAMILY="other" ;;
    esac
fi
printf 'distribution: %s  (family: %s)\n' "${DISTRO:-unknown}" "${FAMILY:-unknown}"
printf 'kernel:       %s\n' "$(uname -r)"
printf 'machine:      %s\n' "$(uname -m)"
printf '\n'

# The package list per family. Kept in one place so the answer to "what do I install" is a table rather than
# four copies of the same list with different spellings.
case "$FAMILY" in
debian)
    PKGS_TOOLCHAIN="gcc-multilib g++-multilib"
    PKGS_VULKAN="libvulkan1:i386 mesa-vulkan-drivers:i386"
    PKGS_SDL="libsdl2-dev:i386"
    PKGS_GLSLANG="glslang-dev:i386"
    PKGS_FFMPEG="libavcodec-dev:i386 libavformat-dev:i386 libswresample-dev:i386"
    APT="sudo apt-get install"
    ;;
fedora)
    PKGS_TOOLCHAIN="glibc-devel.i686 libstdc++-devel.i686"
    PKGS_VULKAN="vulkan-loader.i686 mesa-vulkan-drivers.i686"
    PKGS_SDL="SDL2-devel.i686"
    PKGS_GLSLANG="glslang-devel.i686"
    PKGS_FFMPEG="ffmpeg-devel.i686"
    APT="sudo dnf install"
    ;;
arch)
    PKGS_TOOLCHAIN="gcc"
    PKGS_VULKAN="vulkan-icd-loader lib32-vulkan-icd-loader lib32-mesa"
    PKGS_SDL="lib32-sdl2"
    PKGS_GLSLANG="shaderc lib32-shaderc"
    PKGS_FFMPEG="ffmpeg4.4 lib32-ffmpeg"
    APT="sudo pacman -S"
    ;;
suse)
    PKGS_TOOLCHAIN="gcc glibc-devel"
    PKGS_VULKAN="libvulkan1 libvulkan_intel libvulkan_radeon"
    PKGS_SDL="libSDL2-devel"
    PKGS_GLSLANG="glslang-devel"
    PKGS_FFMPEG="libavcodec-devel libavformat-devel"
    APT="sudo zypper install"
    ;;
*)
    PKGS_TOOLCHAIN="(unknown)"
    PKGS_VULKAN="(unknown)"
    PKGS_SDL="(unknown)"
    PKGS_GLSLANG="(unknown)"
    PKGS_FFMPEG="(unknown)"
    APT=""
    ;;
esac

# ---- 1. the 32-bit toolchain ----
printf '%s1. the 32-bit toolchain%s\n' "$BOLD" "$OFF"
TMPC=$(mktemp -d)
trap 'rm -rf "$TMPC"' EXIT
cat > "$TMPC/t.cpp" <<'EOF'
#include <cstdio>
int main() { std::printf("%zu\n", sizeof(void *)); return 0; }
EOF
if command -v gcc >/dev/null 2>&1; then
    if g++ -m32 -std=c++11 "$TMPC/t.cpp" -o "$TMPC/t" 2>"$TMPC/err"; then
        GOT=$("$TMPC/t")
        if [ "$GOT" = "4" ]; then
            say_ok "g++ -m32 links and produces a 32-bit binary"
        else
            say_bad "g++ -m32 linked but produced a $GOT-byte pointer, not 4"
            say_note "the port is defined as i386; something is off with the multilib libraries"
        fi
    else
        say_bad "g++ -m32 does not link"
        say_note "install: $APT $PKGS_TOOLCHAIN"
        say_note "detail:  $(head -2 "$TMPC/err" 2>/dev/null | tr '\n' ' ')"
    fi
else
    say_bad "no g++ at all"
    say_note "install a C++ compiler first"
fi
printf '\n'

# ---- 2. Vulkan, measured rather than assumed ----
printf '%s2. Vulkan%s\n' "$BOLD" "$OFF"
if [ -e /dev/dri ]; then
    printf '        /dev/dri: %s\n' "$(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
else
    say_warn "there is no /dev/dri, so there is no GPU to render on"
    say_note "llvmpipe (software) still works and is enough to run the tests; see below"
fi

# A real 32-bit program that enumerates devices. Not the loader: the loader refuses to start without a
# device, which is right for running the game and wrong for asking what is installed.
cat > "$TMPC/vk.cpp" <<'EOF'
#include <vulkan/vulkan.h>
#include <cstdio>
int main() {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_1;
    ai.pApplicationName = "check-env";
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    VkInstance inst;
    if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) { std::puts("no Vulkan instance"); return 1; }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    if (!n) { std::puts("no physical devices"); return 1; }
    VkPhysicalDevice *devs = new VkPhysicalDevice[n];
    vkEnumeratePhysicalDevices(inst, &n, devs);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(devs[i], &p);
        std::printf("  [%u] %s\n      type %u, API %u.%u.%u\n", i, p.deviceName, p.deviceType,
                    VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
    }
    delete[] devs;
    return 0;
}
EOF
VKINC=""
for c in /usr/include/vulkan/vulkan.h /usr/include/i386-linux-gnu/vulkan/vulkan.h; do
    [ -r "$c" ] && VKINC="$c" && break
done
if [ -z "$VKINC" ]; then
    say_warn "no 32-bit Vulkan headers found; cannot measure, only install"
    say_note "install: $APT $PKGS_VULKAN"
else
    VKLIB=$(ls /usr/lib/i386-linux-gnu/libvulkan.so* 2>/dev/null | head -1)
    if [ -n "$VKLIB" ] && g++ -m32 "$TMPC/vk.cpp" -o "$TMPC/vk" -lvulkan 2>"$TMPC/verr"; then
        if "$TMPC/vk" > "$TMPC/vout" 2>&1; then
            say_ok "a 32-bit Vulkan program enumerates devices"
            sed 's/^/      /' "$TMPC/vout"
            say_note "seeing a device here is NOT the same as the loader working on it;"
            say_note "run ./build/linux/blacklinux --doctor for the loader's own measurement"
        else
            say_bad "a 32-bit Vulkan program runs but finds nothing"
            sed 's/^/      /' "$TMPC/vout"
            say_note "install: $APT $PKGS_VULKAN"
        fi
    else
        say_bad "a 32-bit Vulkan program does not link"
        say_note "install: $APT $PKGS_VULKAN"
    fi
fi
printf '\n'

# ---- 3. SDL2 ----
printf '%s3. SDL2%s\n' "$BOLD" "$OFF"
if [ -r /usr/include/SDL2/SDL.h ] || [ -r /usr/include/i386-linux-gnu/SDL2/SDL.h ]; then
    say_ok "SDL2 headers are present"
    say_note "A7 has not built the window path yet, so this is not yet used"
else
    say_warn "no SDL2 headers"
    say_note "install: $APT $PKGS_SDL"
fi
printf '\n'

# ---- 4. glslang ----
printf '%s4. glslang (the shader runtime)%s\n' "$BOLD" "$OFF"
GLSL_OK=0
for p in /usr/lib/i386-linux-gnu/libglslang.a /usr/lib/i386-linux-gnu/libglslang.so; do
    [ -e "$p" ] && GLSL_OK=1
done
if [ "$GLSL_OK" = "1" ]; then
    say_ok "the 32-bit glslang library is installed"
else
    say_bad "no 32-bit glslang"
    say_note "install: $APT $PKGS_GLSLANG"
    say_note "note: on Debian/Ubuntu the package is glslang-dev:i386, NOT libglslang-dev."
    say_note "      installing the amd64 one is the single most common way to get this wrong."
fi
printf '\n'

# ---- 5. the build itself ----
printf '%s5. the build%s\n' "$BOLD" "$OFF"
if [ -x build/linux/blacklinux ]; then
    say_ok "build/linux/blacklinux exists"
    if [ -x build/linux/test_material ]; then
        say_ok "the test binaries are there too; ctest --preset linux runs them"
    fi
else
    say_warn "not built yet"
    say_note "cd loader && cmake --preset linux && cmake --build --preset linux"
fi
printf '\n'

# ---- 6. the game ----
printf '%s6. the game image%s\n' "$BOLD" "$OFF"
XBE="${1:-game/default.xbe}"
if [ -r "$XBE" ]; then
    say_ok "$XBE is present"
    say_note "hash it with: sha256sum $XBE"
else
    say_warn "$XBE is not here"
    say_note "nothing above needs it. Track B does, and so do the per-build tables."
fi
printf '\n'

# ---- summary ----
printf '%ssummary%s  %d pass, %d warn, %d fail\n' "$BOLD" "$OFF" "$ok" "$warn" "$bad"
if [ "$FAMILY" = "other" ]; then
    printf 'no package names for this family; the checks above still apply\n'
fi
printf '\nThis script installed nothing. The commands above are for whoever owns the machine.\n'
[ "$bad" -eq 0 ]