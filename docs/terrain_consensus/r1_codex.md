# Round 1 clean RE: Sub Rosa CITY GROUND terrain shaping

Reverser: codex

Status: BLOCKED before target validation.

## Scope compliance

- Intended target: `subrosa.exe.THIS_ONE`
- Required preflight: `list_open_programs` -> `switch_program` -> `get_current_program_info`
- Required investigation mode: Ghidra MCP only, read-only
- Files written: this file only
- Source edits/build/commit/Ghidra renames: none
- Other reversers' files: not read

## Blocker

I could not confirm the active Ghidra program. Every attempted Ghidra MCP call returned:

```text
user cancelled MCP tool call
```

Calls attempted:

- `list_open_programs`
- `list_open_programs` retry
- `get_current_program_info` with `program="subrosa.exe.THIS_ONE"`
- `list_segments` with `program="subrosa.exe.THIS_ONE"`

Because the hard constraint requires exact active-program validation against `subrosa.exe.THIS_ONE`, I stopped before making any reverse-engineering claims.

## Findings

No confirmed terrain findings.

I did not decompile or disassemble:

- `Terrain_ApplyStreetIntersectionHeightPatch @ 0xd3340`
- `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches @ 0xd12e0`
- `UpdateMapTopHeightAndClearOccupancyBit @ 0x3fa40`
- `MapConnector_PlaceStreetSegmentTilesAndSlopes @ 0x20580`
- `Terrain_SetLandSampleWithTileBorderMirroring @ 0xd2db0`

## Key questions

All requested technical answers remain unproven:

1. Corner height source: unproven.
2. Exact patch rectangle: unproven.
3. Feather/smoothing pass count, kernel, weights, normalization, final blend: unproven.
4. Patched intersection subset/order: unproven.
5. World/grid mapping for the city region: unproven.

## Confidence

High confidence only in the blocker report above. No confidence assigned to city-ground behavior because the required Ghidra evidence was unavailable.
