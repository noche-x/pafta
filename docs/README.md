# Pafta documentation

## Using the editor

- [Downloading](../README.md#artifact-builds): beta archives and what they include.
- [Building](../README.md#building): platform requirements and build commands.
- [Using](USAGE.md#open-a-map): opening a map folder.
- [Edit and save](USAGE.md#edit-and-save): controls, save outputs, and precautions.
- [File support and limits](USAGE.md#file-support-and-limits): supported formats and current editing limits.
- [Browser builds](WEB_BUILD.md): Emscripten and loading map folders in a browser.
- [Mesh export](mesh_export.md): exporting glTF from the desktop app.

## Changing the editor

- [Development](../README.md#development): CI, quality checks, coverage, and profiling.
- [CONTRIBUTING](../CONTRIBUTING.md) and the [style guide](../STYLE_GUIDE.md).
- [Beta release record](BETA_RELEASE.md): checks done, release work left, and current limits.
- [File formats](file_formats/README.md): the byte layouts and save behavior in `src/formats.cpp`.
- [AI disclosure](../README.md#ai-disclosure): how AI helped build Pafta and the author's responsibility for the result.
- [License](../README.md#license) and [third-party notices](../THIRD_PARTY_NOTICES.md).

## Slop

[`slop/`](slop/README.md) holds AI-generated reverse-engineering notes about the game's binaries: the `RE_*.md` files, the terrain reports, and the UV investigation. Treat them as leads, not facts.

## A warning about these docs

Most of these docs have not been checked for AI-generated content. AI helped write a lot of them, so read them with that in mind and check anything important against the code or the game.
