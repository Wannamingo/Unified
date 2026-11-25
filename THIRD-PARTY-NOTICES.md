# Third-party notices

The driver retains the original ARMSX2 and bmdhacks MIT license and its source-file notices.
The current driver lineage starts from [the upstream 0.4.0 release](https://github.com/bmdhacks/malisx2/releases/tag/v0.4.0).

Mesa sources are fetched from [the pinned revision](https://github.com/bmdhacks/armsx2-turnip/tree/45ff8fac44d9132118d0dddd2ff49f5d827533fb) and built with the [Mesa patch](patches/mesa-0.4.91.patch).
Their original license identifiers remain in the upstream files. The complete `licenses/` directory is copied into the Android distribution.

The Rust dependencies are pinned by `subprojects/*.wrap`, with versions and original license notices included in the Android distribution.

- `bitflags-2.9.1`. LICENSE-APACHE, LICENSE-MIT.
- `paste-1.0.14`. LICENSE-APACHE, LICENSE-MIT.
- `proc-macro2-1.0.86`. LICENSE-APACHE, LICENSE-MIT.
- `quote-1.0.35`. LICENSE-APACHE, LICENSE-MIT.
- `rustc-hash-2.1.1`. LICENSE-APACHE, LICENSE-MIT.
- `syn-2.0.87`. LICENSE-APACHE, LICENSE-MIT.
- `unicode-ident-1.0.12`. LICENSE-APACHE, LICENSE-MIT, LICENSE-UNICODE.
- `xml-1.2.1`. LICENSE.

## zenithblue PanVK

The experimental LOAD dependency optimisation was adapted from the zenithblue PanVK patch 0119, authored by abhay-byte. [Upstream patch](https://github.com/zenithblue-oss/panvk-kbase-android/commit/078cbd6f0f50b13d5d51038904afb4887b780f42)

## Upstream driver fixes in Unified 0.4.92

The forward-pixel-kill guard, packed RGB image capability restriction,
push-descriptor shadow initialization and CSF UK 1.18/1.19 compatibility
were adapted from the MIT-licensed [0.6.1 source](https://github.com/bmdhacks/malisx2/tree/v0.6.1)
by Brian Degenhardt: `9c51152`, `0e67eaa`, `17b4676` and `a049087`.
