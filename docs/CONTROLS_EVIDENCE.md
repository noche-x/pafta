# Alex control preset evidence

Checked on October 14, 2026 through the running Ghidra MCP bridge in project
`SubRosa Ghidra 38e`. Queries selected each full program path explicitly. No
program bytes, labels, types, comments, analysis state, or saved projects changed.

The bridge exposed its initial tools but did not publish its dynamically loaded
analysis tools to the current tool session. Read-only HTTP GET calls went to the
same MCP plugin's discovered local Unix socket. Both programs were listed and
their selected metadata was checked before the control queries.

## Native editor

- Selected program: `/codex_temp/editor.exe`, x86-64 Windows PE.
- Source file: `/Users/noche/Downloads/editor.exe`, 1,124,864 bytes.
- Source SHA-256: `957af3bd6ea6e990aecbd1f9ba49887a7d1312bee8f6931b12c272087f4608eb`.
- Movement function: `FUN_14006a870`, at `0x14006a870`.
- Ghidra decompilation checks WASD at key-state base `0x1663b96e0`, offsets
  `0x1a`, `0x16`, `0x04`, and `0x07`, while canvas flag `param_6 & 4` is active.
- Shift at offset `0xe1` multiplies movement by 4. Ctrl at `0xe0` multiplies it
  by 0.25 when Shift is absent. With Shift, Ctrl multiplies the already boosted
  speed by another 4, for 16 times the base speed.
- MCP memory reads confirm constants `0x1400ec950 = 1.0`,
  `0x1400ec90c = 0.25`, and `0x1400ec9b4 = 4.0`.

The native editor's MCP disassembly endpoint returned no instructions for this
function. Its instruction inventory reports zero instructions despite providing
decompilation. `llvm-objdump` on the hash-checked source PE independently confirms
the speed branches at `0x14006af21` through `0x14006afaa`, and WASD offsets starting
at `0x14006b004`. No disassembly or analysis command that modifies Ghidra was used.

## Game free camera

- Selected program: `/subrosa.exe.THIS_ONE`, x86-64 Windows PE, image base zero.
- Recorded source path: `/mnt/games/SteamLibrary/steamapps/common/Sub Rosa/subrosa.exe`.
  This source path is unavailable on this host, so a whole-file SHA-256 was not
  checked.
- Movement function: `DebugFreeCamera_UpdateMovement`, at `0x00104ea0`.
- Ghidra normalized function opcode hash:
  `b46c99983ec24b5733120523c58ebc3e9bed93228fc52f3c3afdc354878b27f3`.
  This identifies the queried function, not the entire executable.
- Decompilation and disassembly use level, yaw-relative WASD. They change camera
  Y separately for `0x43f7c974` and `0x43f7c97d`, which are Q and Z relative to
  key-state base `0x43f7c960`. Assembly at `0x0010507a` and `0x001050a0` confirms
  the respective upward addition and downward subtraction.
- SDL's vendored `SDL_scancode.h` confirms these key offsets. The game input
  function at `0x0009f050` writes the same key-state array from input events.

## Pafta behavior

Alex combines the native editor's speed modifier rules with the game's level
WASD and Q/Z vertical controls. Its mouse navigation retains middle-drag pan,
Alt-left orbit, and right-mouse look. This is a deliberate combination of the two
native control paths, not a claim that every editor.exe input mode is duplicated.

Blender keeps middle-drag orbit, Shift-middle pan, Ctrl-middle zoom, and Pafta's
view-relative right-mouse fly controls: WASD, Space/Ctrl for vertical movement,
Shift to slow down, and Z to speed up. Both presets retain per-mode fly speeds.
