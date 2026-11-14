# Third-party notices

The driver retains the original ARMSX2 and bmdhacks MIT license and its source-file notices.
The current driver lineage starts from [the upstream 0.4.0 release](https://github.com/bmdhacks/malisx2/releases/tag/v0.4.0).

Mesa sources are fetched from [the pinned revision](https://github.com/bmdhacks/armsx2-turnip/tree/f59c211100b174191d9df540451e94b2d5cb2b26).
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
