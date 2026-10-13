# Pafta style guide

Write code so a reader can see what runs, when it runs, what state it reads, and what it changes. A whole, readable operation beats both a dense one-liner and a chain of tiny helpers that just pass calls along. Public names and file formats are promises to other code; do not rename them only for style.

## Follow the behavior

Start where the behavior starts: a real entry point or a UI action. Follow the calls through parsing, editor state, rendering, or saving until you know what it returns and what it changes. Keep what you know, what you guess, and what you don't know apart. Code that compiles but never runs tells you nothing about how the editor behaves today.

Keep the condition, the local values, the math, and the result close together, and let the caller show the order of steps. A helper deserves its own name when it does a whole job, holds a rule shared by several places, or wraps an outside API. That can be true even if only one place calls it. Leave simple, clear math inline. Split code apart when the pieces do unrelated things and the split makes each easier to follow.

Arrange files around the ideas a reader will look for. One file format or one render pass can stay in a long file; length alone is not a reason to split. Before adding a wrapper, dependency, or setting, check whether existing code or the standard library already does it. Add options when the current behavior needs them, not before.

Use the project's C++23 style and plain names from the game's world. Short names like `cell`, `slot`, `road`, `portal`, `pos`, and `rot` are fine when the surrounding code makes them clear. Keep plain data as plain structs. Add methods when they protect a rule between fields or manage a change of state. When cases follow different format or lifetime rules, write a separate branch for each.

## Binary files and errors

Map files come from outside the program, so don't trust them. Check sizes and counts before you index or allocate, and that includes multiplying and adding offsets. A cut-off record must never turn into a half-filled asset that Save then writes back with data missing. When an edit promises to leave unknown bytes alone, keep them.

Readers and writers should say which file type, path or record, and condition failed, when they know it. A failed edit must not hand back the original bytes and claim it worked. Catch errors where a load, save, or UI action starts, so you can show a clear message. Don't swallow a failure deep in our own code just to keep the editor running.

A map save is a group of files that belong together. Build the bytes first and replace files as a separate step, check every write, and keep enough detail to explain a save that stopped halfway. Any change to saving needs a test that uses real input and writes to a folder you can look at afterwards.

## State, threads, and rendering

Each piece of editable state should have one owner. When an edit changes a building, block, or buildblock, update its unsaved flag, undo history, saved bytes, and render caches all at once. A second copy of state needs a clear reason and a clear point where it gets refreshed.

Worker threads may parse files or build CPU data, as long as each worker owns what it writes. raylib and OpenGL uploads stay on the render thread. In GPU code, make it obvious where state changes and where it is put back. Don't count on some other draw call to clean up state you left behind.

## Tests and documentation

Test the rule that is most likely to break, especially at a file edge, a version switch, a save path, or a scene conversion. For small writer cases, compare against saved expected bytes. With real files, keep three cases apart: a patch that changes nothing, a full export that matches exactly, and a first export that changes some bytes. A second export that matches the first does not prove the first one lost nothing. Keep checks on in Release builds. Report coverage for the files it actually measures, and say which UI and GPU paths have no tests. A CTest pass shows how the code behaved on this machine. It does not prove the game or a browser will do the same.

Comments should explain a rule, a limit, or a game function that is not obvious from the code. They should not describe the next line. Put format tables and long research notes in `docs/file_formats/`, and mark unknown meanings as unknown. Update the README when something users do changes, and make sure its commands work from the paths it gives.

When you hand off work, say what changed, why, what you checked, and what is still uncertain. For changes that only touch docs, read the final text and look at the diff. A native build does not check Markdown.
