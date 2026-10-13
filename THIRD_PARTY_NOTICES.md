# Third-party notices

Pafta bundles or compiles the dependencies listed below. Their original notices
are in `licenses/` and ship with desktop and browser packages. Pafta's own code
uses the [GPL-2.0-only license](LICENSE). Sub Rosa map and game assets remain
under their owners' terms.

| Component | Version or source revision | Notice |
| --- | --- | --- |
| raylib and its embedded libraries | 5.5 | `raylib.txt`, `GLFW.txt`, `embedded-libraries.txt` |
| Dear ImGui | fd13a1e8923a0a7077b404fc36fd063b25a0c0b5 | `Dear-ImGui.txt` |
| rlImGui | db8239140a68bf9a07f4499d2875d7cefa8fa7e6 | `rlImGui.txt` |
| Font Awesome Free | Font data and icon definitions in the pinned rlImGui source | `Font-Awesome.txt` |
| nativefiledialog-extended | 1.3.0, desktop only | `nativefiledialog-extended.txt` |
| tinygltf, nlohmann/json, stb | 2.9.3, desktop only | `tinygltf.txt`, `nlohmann-json.txt`, `embedded-libraries.txt` |
| DejaVu Sans Mono | `assets/fonts/DejaVuSansMono.ttf` | `DejaVuSansMono.txt`, extracted from the bundled font |

The Font Awesome font uses SIL Open Font License 1.1. Its upstream notice
identifies the license; the full text is in `licenses/OFL-1.1.txt`.
`embedded-libraries.txt` retains license blocks from the fetched headers,
including optional raylib formats that may be compiled into the binary.

System libraries supplied by the operating system are not included in the ZIP.
The optional easy_profiler developer build is not part of beta packages.
