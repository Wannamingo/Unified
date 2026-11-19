# Unified 0.4.91 Preview

Unified is a Vulkan driver for Arm Mali GPUs on Android with the Arm `kbase` kernel driver. The application must support importing custom GPU driver packages.

The 0.4.91 preview uses the Vulkan 1.3 Lab configuration. The exact packaged binary was checked on RG477M (Mali-G615), RG476H (Mali-G57), and RG Rotate (Mali-G52). See [release notes](RELEASE-NOTES-0.4.91.md) for the scope of the tests and the opt-in flags.

## Installation

Download the [driver ZIP](https://github.com/Wannamingo/Unified/releases/tag/v0.4.91) and import it intact through the application's GPU driver settings. In HakuX-Mali, select **Settings → GPU driver → Add a driver**. Select the imported driver and restart the game. You can switch back by selecting **System** and restarting again.

## Build from source

```sh
git clone https://github.com/Wannamingo/Unified.git
cd Unified
git checkout v0.4.91
git init mesa
git -C mesa fetch --depth 1 https://github.com/bmdhacks/armsx2-turnip.git 45ff8fac44d9132118d0dddd2ff49f5d827533fb
git -C mesa checkout --detach FETCH_HEAD
git -C mesa apply ../patches/mesa-0.4.91.patch
```

The build needs Meson 1.7 or newer, Ninja, Python with Mako and PyYAML, a native C/C++ toolchain, Rust with the `aarch64-linux-android` target, and bindgen with libclang. The preview used NDK `29.0.14206865` with API 31. For a Linux-hosted cross build, copy `cross/android-aarch64.ini` to `cross/android-local.ini`, then set the NDK path and the API to 31.

```sh
meson setup build-android --cross-file cross/android-local.ini \
  --buildtype=release -Db_ndebug=true -Dmeasurement=true \
  -Dunified_api12_lab=true -Dunified_api13_lab=true \
  -Dunified_validation_lab=true -Dunified_geometry_lab=true \
  -Dunified_tessellation_lab=true
meson compile -C build-android
```

The build produces `build-android/src/libvulkan_unified.so`. The package's `meta.json` identifies the library to load.

Unified retains the MIT license and notices of ARMSX2 and bmdhacks. See [LICENSE](LICENSE) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
