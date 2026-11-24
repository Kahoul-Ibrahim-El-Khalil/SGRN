# SGRN build (xmake)

CMake has been removed; xmake is the only build system.

| Former CMake preset          | xmake                                                                         |
| ---------------------------- | ----------------------------------------------------------------------------- |
| `linux-static-release`       | `xmake f -p linux -m release -y && xmake`                                     |
| `linux` (dev, shared libs)   | `xmake f -p linux -m debug -k shared -y && xmake`                             |
| `win64-static-release`       | `xmake f -p mingw -a x86_64 -m release -y && xmake`                           |
| `linux-arm64-static-release` | `xmake f -p cross -a arm64 --cross=aarch64-linux-gnu- -m release -y && xmake` |
| `*-deps*` workflows          | automatic: `xmake require` builds/caches packages (`xmake/packages/`)         |

Options (`xmake f --name=value`): `datastore`, `torch`, `opc`, `tests`, `pch`, `native`, `ucrt`, `web`,
`sanitize`, `torch_dir`, `torch_device`, `datastore_release`, `datastore_debug`, `auto_codegen`.
Tests: compiled by default (`--tests=y`); run with `xmake test -v`. Tasks: `xmake stage`, `xmake generate-views`, `xmake crud-generate`.
Outputs: compiled binaries & test executables -> `.build/<plat>/<arch>/<mode>/`, generated headers -> `.build/gen/`.

LibTorch: found via `--torch_dir`, `$LIBTORCH`, conda, or the cache in `.prefix/libtorch-*`; otherwise the official prebuilt zip is downloaded there (`SGRN_TORCH_URL` overrides, `SGRN_TORCH_SHA256` verifies). Disable with `--torch=n`.
