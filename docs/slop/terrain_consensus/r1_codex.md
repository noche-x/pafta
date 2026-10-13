# City terrain research, Codex round 1

Reverser: codex

Status: BLOCKED before checking the target.

## Rules followed

- Target: `subrosa.exe.THIS_ONE`
- Required first steps: `list_open_programs` -> `switch_program` -> `get_current_program_info`
- Required method: Ghidra MCP only, read-only
- Files written: this file only
- Source edits, builds, commits, or Ghidra renames: none
- Other reversers' files: not read

## What blocked it

I could not confirm which program Ghidra had open. Every Ghidra MCP call I tried returned:

```text
user cancelled MCP tool call
```

Calls tried:

- `list_open_programs`
- `list_open_programs` again
- `get_current_program_info` with `program="subrosa.exe.THIS_ONE"`
- `list_segments` with `program="subrosa.exe.THIS_ONE"`

The rules required confirming that the open program was exactly `subrosa.exe.THIS_ONE`, so I stopped before making any claims about the game.

## Findings

None confirmed.

I did not decompile or disassemble:

- `Terrain_ApplyStreetIntersectionHeightPatch @ 0xd3340`
- `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches @ 0xd12e0`
- `UpdateMapTopHeightAndClearOccupancyBit @ 0x3fa40`
- `MapConnector_PlaceStreetSegmentTilesAndSlopes @ 0x20580`
- `Terrain_SetLandSampleWithTileBorderMirroring @ 0xd2db0`

## Questions

None of the questions were answered:

1. Where corner heights come from: not proven.
2. The exact patch rectangle: not proven.
3. Smoothing pass count, kernel, weights, normalization, and final blend: not proven.
4. Which intersections are patched, and in what order: not proven.
5. How world and grid coordinates map in the city: not proven.

## Confidence

The only thing I am sure of is the blocker above. I make no claims about the city ground because the Ghidra evidence was not available.
