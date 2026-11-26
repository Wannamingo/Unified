# Unified 0.5.0

Unified is a Vulkan driver for Arm Mali GPUs on Android with the Arm `kbase` kernel driver. The application must support importing custom GPU driver packages.

Unified 0.5.0 uses the Vulkan 1.4 Lab configuration and adds custom sampler border colors. See the [release notes](RELEASE-NOTES-0.5.0.md) for device-specific behavior and test coverage.

## Installation

Download the [driver ZIP](https://github.com/Wannamingo/Unified/releases/tag/v0.5.0) and import it intact through the application's GPU driver settings. In BoxForever, select **Settings → GPU driver → Add a driver**. Select the imported driver and restart the game. To return to the stock driver, select **System** and restart the game.

## Build from source

```sh
git clone https://github.com/Wannamingo/Unified.git
cd Unified
git checkout v0.5.0
git init mesa
git -C mesa fetch --depth 1 https://github.com/bmdhacks/armsx2-turnip.git 45ff8fac44d9132118d0dddd2ff49f5d827533fb
git -C mesa checkout --detach FETCH_HEAD
git -C mesa apply ../patches/mesa-0.5.0.patch
```

The build needs Meson 1.7 or newer, Ninja, Python with Mako and PyYAML, a native C/C++ toolchain, Rust with the `aarch64-linux-android` target, and bindgen with libclang. This release uses NDK `29.0.14206865` with API 31. For a Linux-hosted cross build, copy `cross/android-aarch64.ini` to `cross/android-local.ini`, then set the NDK path and the API to 31.

```sh
meson setup build-android --cross-file cross/android-local.ini \
  --buildtype=release -Db_ndebug=true -Dmeasurement=true \
  -Dunified_api12_lab=true -Dunified_api13_lab=true -Dunified_api14_lab=true \
  -Dunified_validation_lab=true -Dunified_geometry_lab=true \
  -Dunified_tessellation_lab=true
meson compile -C build-android
```

The build produces `build-android/src/libvulkan_unified.so`. The package's `meta.json` identifies the library to load.

Unified retains the MIT license and notices of ARMSX2 and bmdhacks. See [LICENSE](LICENSE) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
