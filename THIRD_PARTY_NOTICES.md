# Third-party software

| component | where | license |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | `third_party/imgui/` (vendored) | MIT, see `third_party/imgui/LICENSE.txt` |
| [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) | `third_party/imguizmo/` (vendored) | MIT, see `third_party/imguizmo/LICENSE` |
| [Panda3D](https://www.panda3d.org/) | linked at build time; its runtime DLLs are bundled by the Windows installer | Modified BSD license |
| zlib, OpenSSL | used through Panda3D (compressed / encrypted `.pdna`) | zlib license / Apache-2.0 (OpenSSL 3) |

The `.pdna` reader/writer was written from the binary layout used by
[libpandadna](https://github.com/loblao/libpandadna) (BSD-3-Clause); no libpandadna code is included.
Output was checked to be byte-identical to libpandadna's `compile.py` on the sample files.
