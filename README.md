# Unified

Unified is a Vulkan driver for Arm Mali GPUs on the stock Arm `kbase` kernel driver. On Android, it is loaded by an application that supports custom GPU driver packages, such as HakuX-Mali.

[Download Unified 0.4.53](https://github.com/Wannamingo/Unified/releases/tag/v0.4.53)

| Device | GPU | GPU validation |
| --- | --- | --- |
| Anbernic RG476H | Mali-G57 | 156 vertex-divisor scenarios |
| Anbernic RG477M | Mali-G615 | 156 vertex-divisor scenarios |
| Anbernic RG Rotate | Mali-G52 | 156 vertex-divisor scenarios |

The Android package targets ARM64 and Android 12 (API 31) or newer. The driver reports Vulkan 1.1.366 and exposes additional extensions. This version has no Vulkan conformance certification. Application and firmware compatibility can vary.

Unified 0.4.53 enables `VK_KHR_vertex_attribute_divisor` on the verified GPUs, including zero and non-power-of-two instance divisors through 65535. A nonzero `firstInstance` with a divisor other than 1 is not supported. See the release notes for the test scope.

## Install / Установка

1. Download the driver ZIP from the release page. Keep the archive intact.
2. In a compatible app, open its GPU-driver settings and import the ZIP. In HakuX-Mali, use **Settings → GPU driver → Add a driver**.
3. Select **Unified 0.4.53** and restart the game. To return to the device driver, select **System** and restart the game again.

Скачайте ZIP из Releases. В HakuX-Mali откройте **Настройки → Драйвер GPU → Добавить драйвер**, выберите ZIP, затем **Unified 0.4.53** и перезапустите игру. Для возврата выберите **Системный драйвер** и снова перезапустите игру. Версия HakuX-Mali 1.7.8 распознаёт Unified. Проверенная 1.7.1 на RG Rotate отклоняла драйвер с именем Unified.

## Build from source

Use Mesa at the revision pinned in `meson.build`. The public build uses only the pinned Mesa sources and its license notices, while `mesa-glue/` defines what to compile.

```sh
git clone https://github.com/Wannamingo/Unified.git
cd Unified
git checkout v0.4.53
git init mesa
git -C mesa fetch --depth 1 https://github.com/bmdhacks/armsx2-turnip.git f59c211100b174191d9df540451e94b2d5cb2b26
git -C mesa checkout --detach FETCH_HEAD
```

The build needs Meson 1.7+, Ninja, Python with Mako and PyYAML, a native C/C++ toolchain, Rust with the `aarch64-linux-android` target, and bindgen with libclang. The published Android build used NDK `29.0.14206865`, API 31 and `-Dmeasurement=true`. For a Linux-hosted cross build, copy `cross/android-aarch64.ini` and set `ndk` and `api = '31'` in the copy before configuring Meson.

```sh
meson setup build-android --cross-file cross/android-local.ini --buildtype=release -Db_ndebug=true -Dmeasurement=true -Dunified_api12_lab=false
meson compile -C build-android
```

The build produces `build-android/src/libvulkan_unified.so`. The Android ZIP carries the tested binary under the loader-compatible name `libvulkan_malisx2.so`.

## Credits and licensing

Unified is based on the MIT-licensed work of ARMSX2 and bmdhacks and uses Mesa. See [LICENSE](LICENSE) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). The release archive includes the dependency license texts. Credit for the JM tiler-heap changes is recorded in the 0.4.51 release notes.
