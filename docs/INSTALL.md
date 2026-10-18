# Installing the toolchain, per distribution

This is the short version: what to install, per distribution, before `loader/` will build. For how to get
the game itself and what state the port is in, read [`CARA-PASANG-GAME.md`](CARA-PASANG-GAME.md).

Two things to know before reading further.

**The port is i386, not x86-64.** Every thunk, every stack frame, and every address in the game's own
headers assumes a 32-bit world. A 64-bit loader is not a faster loader, it is a different program with
different bugs — so 32-bit libraries are required, and a package name without an `:i386` suffix or an
`.i686` suffix is the wrong package.

**Some of what follows has been run, and some has not.** Section 1 was run on the machine this was written
on. Sections 2 to 4 were written from the distributions' own naming conventions and **have not been run**.
They are labelled. The failure mode for an unverified package name is `no such package`, which announces
itself; but "unverified" here means unverified, and the labels are not decoration.

After installing anything, run:

```
loader/tools/check-env.sh
```

It installs nothing, it checks, and it prints the command for whatever it found missing.

---

## 1. Debian, Ubuntu (verified on this machine)

```
sudo apt-get install gcc-multilib g++-multilib
sudo apt-get install libvulkan1:i386 mesa-vulkan-drivers:i386
sudo apt-get install glslang-dev:i386
sudo apt-get install libsdl2-dev:i386
```

Then:

```
cd loader
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
```

Two notes specific to this family, both of them learned the hard way:

- **The shader package is `glslang-dev:i386`.** It is not `libglslang-dev`, and it is not the amd64
  `glslang-tools`. Installing the amd64 one gives you a `glslangValidator` that cannot run in a 32-bit
  process, which is the failure you get when the build picks up a host compiler for build-time shaders.
- **`mesa-vulkan-drivers:i386` is what provides llvmpipe.** Without it a 32-bit process can see no
  Vulkan device at all on a machine whose only GPU is Intel's, and the failure reads as "Vulkan is not
  installed" when in fact only the 64-bit half is.

## 2. Fedora (not verified)

```
sudo dnf install glibc-devel.i686 libstdc++-devel.i686
sudo dnf install vulkan-loader.i686 mesa-vulkan-drivers.i686
sudo dnf install glslang-devel.i686
sudo dnf install SDL2-devel.i686
```

The `.i686` suffix is the whole of the difference from section 1.

## 3. Arch (not verified)

```
sudo pacman -S gcc
sudo pacman -S vulkan-icd-loader lib32-vulkan-icd-loader lib32-mesa
sudo pacman -S shaderc lib32-shaderc
sudo pacman -S lib32-sdl2
```

Arch keeps 32-bit libraries in `lib32-*` packages rather than in a multilib repository. `shaderc` is the
package that provides both the compiler and the runtime glslang, which is why it is listed where the other
distributions list `glslang-dev`.

## 4. openSUSE (not verified)

```
sudo zypper install gcc glibc-devel
sudo zypper install libvulkan1 libvulkan_intel libvulkan_radeon
sudo zypper install glslang-devel
sudo zypper install libSDL2-devel
```

openSUSE names the driver packages per vendor rather than shipping one metapackage, so software rendering
may need `libvulkan_llvmpipe` added by hand.

---

## What "verified" would mean, and why it is not yet true

The A11 gate asks for these instructions to be verified on at least two distributions. One machine cannot
do that: a container of the second distribution would test the package names, but not the loader, and the
package names are the part most likely to be wrong in a way that matters.

What would close it, in order of cost:

1. A second machine on any of the above. Run `check-env.sh`, paste the output.
2. A container for the second distribution, with `/dev/dri` passed through and `mesa-vulkan-drivers`
   installed. Enough for the package names, not enough for the loader.
3. Nothing, and accept that this port ships with instructions verified on one distribution and clearly
   labelled on three others.

Option 3 is a legitimate answer. It is worse than 1 or 2 and better than claiming verification that did not
happen.

## How this port is distributed

Not decided yet, and the decision is A11's last item. The three candidates and what each costs:

| Form | What it solves | What it costs |
|---|---|---|
| **tarball** (build from source) | nothing about the 32-bit toolchain; the user already has a compiler or knows how to get one | every user hits the multilib problem themselves, which is the single most common way this fails |
| **AppImage** | bundles the loader, SDL2 and the Vulkan loader; still needs the GPU driver from the host, which it must | the build has to produce one artifact per glibc version, and a stale AppImage against a new driver is a bug report nobody can reproduce |
| **container** | the toolchain problem disappears entirely | needs `/dev/dri` and a 32-bit-capable kernel; the GUI path is friction; and it does not help with the driver crash in A0 |

The measurement that should decide it is A0, not taste. A0 is open because the 32-bit Intel driver dies
inside `vkQueueSubmit`, and it does so in a ten-line program that has nothing to do with this port — so on
some machines the packaged form cannot work at all until that is understood. Distributing before A0 closes
means shipping something whose first-run experience on a common machine is a crash.