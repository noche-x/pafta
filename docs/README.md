# Pafta documentation

## Using the editor

- [Downloading](../README.md#beta-builds): beta archives and what they include.
- [Building](../README.md#building): platform requirements and build commands.
- [Using](../README.md#open-a-map): opening a map folder.
- [Edit and save](../README.md#edit-and-save): controls, save outputs, and precautions.
- [File support and limits](../README.md#file-support-and-limits): supported formats and current editing limits.
- [Browser builds](WEB_BUILD.md): Emscripten and loading map folders in a browser.
- [Mesh export](mesh_export.md): exporting glTF from the desktop app.

## Changing the editor

- [Development](../README.md#development): CI, quality checks, coverage, and profiling.
- [CONTRIBUTING](../CONTRIBUTING.md) and the [style guide](../STYLE_GUIDE.md).
- [Beta release record](BETA_RELEASE.md): checks done, release work left, and current limits.
- [File formats](file_formats/README.md): the byte layouts and save behavior in `src/formats.cpp`.
- [Terrain generation](terrain_generation.md): the newest findings about the game's terrain, which the scene code uses. Where it disagrees with the older terrain notes, this one is right.
- [AI disclosure](../README.md#ai-disclosure): how AI helped build Pafta and the author's responsibility for the result.
- [License](../README.md#license) and [third-party notices](../THIRD_PARTY_NOTICES.md).

## Older research notes

The `RE_*.md` files, `terrain_consensus/`, and the [UV investigation](uv_bug_investigation.md) keep earlier work on the game's binaries and on porting decisions. Some of them contain guesses that were corrected later, in the same file or in `terrain_generation.md`. Their source line numbers and their remarks about "the current port" describe the code at the time, not the editor today.

When editing these notes, keep their addresses, formulas, code snippets, and notes about how sure they are. The game's executables and some decompiled code are not in this checkout. A trace from the game, a Pafta test, and a side-by-side comparison with the game are three different kinds of evidence.
