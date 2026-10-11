# Pafta style guide

Write code so a reader can see what runs, under which condition, which state it uses, and what it changes. Prefer a complete, readable operation over either a dense shortcut or a chain of one-line forwarding helpers. Existing public names and file formats are contracts; do not rename them for style alone.

## Follow the behavior

Start at a real entry point or UI action. Follow the call through parsing, editor state, rendering, or saving until its result and side effects are clear. Keep facts, inferences, and unknowns separate. A code path that compiles but never runs does not establish current editor behavior.

Put the condition, useful locals, calculation, and effect near each other. Let the caller show the order of work. A helper earns its name when it performs a whole operation, owns a shared rule, or contains an API boundary. Do not split a simple calculation only to shorten a function. Likewise, do not keep unrelated work in one function merely because it runs in the same frame.

Use the project's C++23 style and plain domain names. `cell`, `slot`, `road`, `portal`, `pos`, and `rot` are useful when scope supplies the rest. Represent plain data plainly; add methods when they enforce a relationship or coordinate a state change. Use explicit branches where cases carry different format or lifecycle rules.

## Binary files and errors

Map files are input from outside the program. Check sizes and counts before indexing or allocating, including multiplication and offset arithmetic. A truncated record must not become a plausible partial asset that Save can write back with missing data. Preserve unknown bytes when a focused edit promises to leave them intact.

Readers and writers should report the file type, path or record, and failing condition where that context is available. A failed edit must not return unchanged bytes and report success. Catch errors at the load, save, or UI boundary to show a useful message; do not hide a failure inside owned code to keep the editor running.

Treat a map save as a set of related files. Keep serialization separate from replacing output files, check write results, and leave enough context to diagnose a partial save. Changes to save behavior need a test with real input and an output directory that can be inspected.

## State, threads, and rendering

Keep one owner for mutable editor state. If an edit changes a building, block, or buildblock, update the matching dirty state, undo history, serialized bytes, and render caches together. A second copy of state needs an explicit reason and a clear refresh point.

Worker threads may parse or build CPU data when each worker owns its output. Keep raylib and OpenGL uploads on the render thread. For GPU code, make state changes and restoration visible at the operation boundary; do not assume another draw path will repair state left behind.

## Tests and documentation

Test the rule that could break, especially at a file boundary, version switch, save path, or scene conversion. Use frozen expected bytes for small writer cases. For real files, distinguish an exact no-op patch, an exact full export, and a first export that normalizes bytes; a stable second export does not make the first one lossless. Keep checks active in Release. Report coverage for the named file set and call out untested UI and GPU paths. A CTest pass is evidence for the exercised host behavior, not proof that the game renderer or browser behaves the same way.

Comments should explain a non-obvious rule, limit, or native source, not narrate the next line. Put format tables and longer recovery notes in `docs/file_formats/`; label unknown semantics as unknown. Update the README when a user-visible workflow changes, and keep its commands executable from the paths it names.

Before handoff, say what changed, why it changed, what you checked, and what remains uncertain. For documentation-only edits, read the final text and inspect the diff; do not run a native build just to validate Markdown.
