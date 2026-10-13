# How Sub Rosa draws and queries terrain at runtime

Target: `subrosa.exe.THIS_ONE` (image base 0, x86-64 LE)

Scope: how terrain is streamed, drawn, culled, queried, and collided with while the game runs. These are the drawing and collision paths traced in Ghidra. Addresses and snippets show where each step comes from, and anything not worked out is marked below.

## 1. Streaming

### 1.1 How the world is split up

Runtime terrain is split into 64 super-chunks in an 8×8 grid. Each super-chunk is split again into a 16×16 grid of sub-chunks (256 in all). That makes 64 × 256 = 16 384 chunks in the world.

- Chunk data (world origin, tile coordinates, and so on) is in a 0xd08-byte (3336) struct per super-chunk, indexed by the super-chunk number.
- In `Terrain_SelectAndRefreshVisibleChunkSlotsAroundPoint @ 0xd26a0`, the outer loop goes `0..63` over the super-chunks. For each, it goes over a 16×16 grid of sub-chunks (`0..15` inner, `0..15` outer).

```c
// Terrain_SelectAndRefreshVisibleChunkSlotsAroundPoint @ 0xd26a0 (decompile excerpt)
iVar16 = 0;
lVar19 = 0;
local_res10 = &DAT_40e6f620;           // visibility byte array (1 byte per super-chunk + 256 LOD bytes)
do {
    ...
    iVar20 = 0;
    lVar13 = 0;
    pfVar15 = pfVar21 + 10;
    do {
        iVar14 = 0;
        lVar12 = 0;
        do {
            ... // per sub-chunk distance test
            (&DAT_40e6f624)[lVar13 + lVar19 + lVar12] = uVar17;   // store selected LOD (0..6)
            ...
            lVar12 = lVar12 + 1;
        } while (lVar12 < 0x10);      // 16 sub-chunks X
        iVar20 = iVar20 + 0x10;
        lVar13 = lVar13 + 0x10;
    } while (iVar20 < 0x100);         // 16 sub-chunks Z
    ...
    local_res10 = local_res10 + 0x101; // stride = 257 bytes per super-chunk
    iVar16 = iVar16 + 1;
    pfVar21 = pfVar21 + 0xd08;         // next super-chunk struct
    lVar19 = lVar19 + 0x101;
} while (iVar16 < 0x40);               // 64 super-chunks
```

### 1.2 Distances for each detail level

`Terrain_SelectAndRefreshVisibleChunkSlotsAroundPoint` works out the squared distance from the given camera point to the centre of each sub-chunk and picks one of 7 detail levels (0 is the most detailed, 6 the least).

The limits are read from `.rdata` as an array of IEEE-754 floats:

| Address | Raw LE | Value | Meaning |
|---|---|---|---|
| `0x278aec` | `00 00 80 48` | 262 144.0 | LOD0 limit (< 512 world units) |
| `0x278af4` | `00 00 10 49` | 589 824.0 | LOD1 limit (< about 768) |
| `0x278afc` | `00 00 80 49` | 1 048 576.0 | LOD2 limit (< 1024) |
| `0x278b00` | `00 00 10 4a` | 2 359 296.0 | LOD3 limit (< about 1536) |
| `0x278b04` | `00 00 80 4a` | 4 194 304.0 | LOD4 limit (< 2048) |
| `0x278b08` | `00 00 10 4b` | 9 437 184.0 | LOD5 limit (< about 3072) |
| `0x278b0c` | `00 00 80 4b` | 16 777 216.0 | LOD6, the furthest you can see (< 4096) |

```c
// Terrain_SelectAndRefreshVisibleChunkSlotsAroundPoint @ 0xd26a0
fVar23 = *param_1 - ((float)((int)pfVar15[-1] << 7) + pfVar21[-2] + fVar1);  // delta X
fVar22 = param_1[2] - ((float)((int)*pfVar15 << 7) + *pfVar21 + fVar1);      // delta Z
fVar22 = fVar23*fVar23 + (param_1[1] - pfVar21[-1])*(param_1[1] - pfVar21[-1]) + fVar22*fVar22;
if (fVar3 <= fVar22) {
    if (fVar22 < fVar4)      uVar10 = 1;   // LOD1
    else if (fVar22 < fVar5) uVar10 = 2;   // LOD2
    else if (fVar22 < fVar6) uVar10 = 3;   // LOD3
    else if (fVar22 < fVar7) uVar10 = 4;   // LOD4
    else if (fVar22 < fVar8) uVar10 = 5;   // LOD5
    else { uVar10 = 6; uVar17 = 6; }        // LOD6
} else {
    uVar10 = 0;                               // LOD0
}
```

If the chosen level has no free slots, the code tries the next less detailed level until it finds one (`do { ... uVar10 = uVar10 + 1; } while (...)`).

### 1.3 Handing out and reusing slots

Each detail level has its own ring of slots.

- Slot data for level *n* starts at `0x410ff780 + n * 0x18018`.
- Each entry is 12 bytes (3 dwords): chunk id, sub-chunk id, age.
- `g_terrainChunkSlotsLod0Count` (and its 6 siblings, `0x6006` dwords apart) holds the number of slots for each level.
- `DAT_410ff794 + n * 0x18018` holds the last slot used (a cursor for reusing the oldest).

`Terrain_RebuildChunkSlot @ 0xd0c10` picks a slot like this:

```c
// Terrain_RebuildChunkSlot @ 0xd0c10 (excerpt)
iVar4 = (&g_terrainChunkSlotsLod0Count)[lVar18 * 0x6006];   // slot count for this LOD
uVar32 = *(uint *)(&DAT_410ff794 + lVar13) + 1U & iVar4 - 1U; // advance cursor, wrap at power-of-two
*(uint *)(&DAT_410ff794 + lVar13) = uVar32;
if (0 < iVar4) {
    do {
        if ((int)(&DAT_410ff7a0)[((longlong)(int)uVar32 + lVar18 * 0x2002) * 3] < 1) break;
        uVar32 = uVar32 + 1 & iVar4 - 1U;   // wrap-around search for free/stale slot
        iVar30 = iVar30 + 1;
        *(uint *)(&DAT_410ff794 + lVar13) = uVar32;
    } while (iVar30 < iVar4);
}
```

A slot's age is set to 4 when it is rebuilt. Every frame, `Terrain_SelectAndRefreshVisibleChunkSlotsAroundPoint` lowers every age above 0 by 1, and slots with `age == 0` can be reused.

### 1.4 Making vertices for a slot

`Terrain_RebuildChunkSlot` walks a regular grid over the 128×128-cell chunk in steps of `1 << LOD`:

```c
iVar21 = 1 << ((byte)param_3 & 0x1f);   // grid step = 2^LOD
iVar4  = iVar21 - 1;                    // step-1
```

The loops go `-1 .. 0x82` (0..129 in practice), so the vertex counts are:

| LOD | Step | Vertices per side | Total vertices |
|---|---|---|---|
| 0 | 1 | 129 | 16 641 |
| 1 | 2 | 65 | 4 225 |
| 2 | 4 | 33 | 1 089 |
| 3 | 8 | 17 | 289 |
| 4 | 16 | 9 | 81 |
| 5 | 32 | 5 | 25 |
| 6 | 64 | 3 | 9 |

For each grid vertex it reads the global 2049×2049 heightfield through `g_landTileHeightChunks[superChunk * 0x684]`, works out the normal from the difference between neighbouring heights, and writes the position and normal together into a CPU scratch buffer at `DAT_40e7f720`.

Vertex format (confirmed by the `glVertexAttribPointer` calls):

- Attribute 0 (position): 3× `GL_FLOAT`, stride 24 bytes, offset 0
- Attribute 1 (normal): 3× `GL_FLOAT`, stride 24 bytes, offset 12

The finished vertex buffer is uploaded with `glBufferSubData`:

```c
(*glBufferSubDataProc)(GL_ARRAY_BUFFER,
    (longlong)(int)((&g_terrainChunkSlotsLod0IndexBufferStride)[lVar18 * 0x6006] * uVar32),
    (longlong)(local_f8 * 6 * 4),               // vertexCount * 6 floats * 4 bytes
    &DAT_40e7f720);
```

`local_f8` is the total vertex count (for example 16641 at LOD0).

### 1.5 GL buffer banks for streaming

`Render_InitTerrainStreamingBuffers @ 0xc2db0` makes six main cache banks and one shared vertex buffer:

```asm
; Render_InitTerrainStreamingBuffers @ 0xc2db0 (disassembly excerpt)
000c2de2: MOV dword ptr [RBX + -0xc02018], 0x8892   ; GL_ARRAY_BUFFER
000c2dec: CMP RSI, 0x2
000c2df0: JC  0x000c2e03
000c2df2: MOV dword ptr [RBX + -0xc02018], 0x8893   ; GL_ELEMENT_ARRAY_BUFFER
000c2dfc: MOV EAX, 0x40
000c2e01: JMP 0x000c2e08
000c2e03: MOV EAX, 0x80
000c2e08: MOV dword ptr [RBX + -0xc02010], EAX      ; slot count (0x80 for verts, 0x40 for indices)
000c2e13: MOV dword ptr [RBX + -0xc0200c], 0x80000  ; per-slot size = 0x80000 bytes (524 288)
...
000c2f22: CMP RSI, 0x6
000c2f26: JL  0x000c2dd4                            ; loop 0..5  (6 banks)
```

In short:

| Bank | GL target | Slots | Slot size | Total | Use |
|---|---|---|---|---|---|
| 0 | `GL_ARRAY_BUFFER`   | 0x80 | 0x80000 | 64 MiB | Vertex data |
| 1 | `GL_ARRAY_BUFFER`   | 0x80 | 0x80000 | 64 MiB | Vertex data |
| 2 | `GL_ELEMENT_ARRAY_BUFFER` | 0x40 | 0x80000 | 32 MiB | Index data |
| 3 | `GL_ELEMENT_ARRAY_BUFFER` | 0x40 | 0x80000 | 32 MiB | Index data |
| 4 | `GL_ELEMENT_ARRAY_BUFFER` | 0x40 | 0x80000 | 32 MiB | Index data |
| 5 | `GL_ELEMENT_ARRAY_BUFFER` | 0x40 | 0x80000 | 32 MiB | Index data |
| extra | `GL_ARRAY_BUFFER` | 1 | 0x80000 | 512 KiB | Shared terrain streaming buffer |

The index buffers are made once ahead of time; the code never calls `glBufferSubData` on them while running. Only vertex data is streamed for each chunk. Each vertex slot is a fixed 0x80000-byte area. The real vertex data is smaller (for example 399 kB at LOD0), and the rest is unused.

`glDrawElementsBaseVertex` lets every chunk slot share the same index pattern, shifted by `baseVertexStep * slotIndex`.

Not worked out: the draw loop (`RenderLandscapeVisibleChunkPasses`) uses 7 detail levels (0..6), but only 6 main banks are set up here. Static analysis alone could not show whether LOD6 uses one of the existing banks or the extra shared buffer.

## 2. Drawing each chunk

`RenderLandscapeVisibleChunkPasses @ 0xd22e0`, called from `RenderWorld @ 0x11fd00`, runs the terrain drawing.

### 2.1 Shader and texture setup (first pass only)

When `param_1 == 0` (the colour, or main, pass):

```c
(*glUseProgramProc)(DAT_115ad740);                         // terrain shader
(*glUniform3fvProc)(DAT_115ad754, 1, &DAT_4417de78);       // light/dir uniforms
(*glUniform3fvProc)(DAT_115ad758, 1, &DAT_44178d40);
(*glUniform3fvProc)(DAT_115ad75c, 1, &DAT_44178d50);
(*glUniform3fvProc)(DAT_115ad760, 1, &camera);
(*glUniformMatrix4fvProc)(DAT_115ad764, 1, 0, &more4x4Matrix);
(*glUniform1iProc)(DAT_115ad7e0, 1);                       // sampler toggle

// texture binds
(*_glBindTexture)(GL_TEXTURE_2D, textures[g_texLandscapeColor].glTextureID);
glActiveTexture(GL_TEXTURE1);  ... g_texLandscapeDetail ...
glActiveTexture(GL_TEXTURE2);  ... g_texLandscapeColor ...
glActiveTexture(GL_TEXTURE4);  ... DAT_43ebe408 ...
```

Every pass binds the landscape mask texture on texture unit 3 (`GL_TEXTURE3`).

### 2.2 The chunk draw loop

```c
// RenderLandscapeVisibleChunkPasses @ 0xd22e0 (decompile excerpt)
local_res10 = 0;          // super-chunk index
local_res18 = 0;          // super-chunk struct offset
local_res20 = &DAT_40e6f624;   // LOD array for current super-chunk
do {
    if (local_res20[-1] == 0) {          // visibility flag at DAT_40e6f620
        RenderLandscapeChunkIndexBufferPass(local_res10);   // fallback prebuilt path
    } else {
        lVar5 = 0x10;
        piVar2 = local_res20;
        lVar4 = local_res18;
        do {
            lVar3 = 0x10;
            do {
                lVar6 = (longlong)*piVar2;              // LOD level (0..6)
                iVar1 = *(int *)((longlong)&DAT_411bf874 + (lVar6 + lVar4) * 4); // slot index
                if (iVar1 != -1) {
                    (*glBindBufferProc)(GL_ELEMENT_ARRAY_BUFFER,
                        *(undefined4 *)(&DAT_43e9b0c4 + (longlong)(int)(&DAT_43e98c4c)[lVar6] * 8));
                    (*glBindBufferProc)(GL_ARRAY_BUFFER,
                        *(undefined4 *)(&DAT_43e9b0c0 + (longlong)(int)(&DAT_43e9908c)[lVar6] * 8));
                    (*glVertexAttribPointerProc)(0, 3, GL_FLOAT, 0,
                        *(int *)(&DAT_410ff780 + lVar6 * 0x18018) << 2, 0);
                    (*glVertexAttribPointerProc)(1, 3, GL_FLOAT, 0,
                        *(int *)(&DAT_410ff780 + lVar6 * 0x18018) << 2, 0xc);
                    (*glDrawElementsBaseVertexProc)(GL_TRIANGLES,
                        (&DAT_410ff784)[lVar6 * 0x6006] * 3, GL_UNSIGNED_INT, 0,
                        (&DAT_410ff78c)[lVar6 * 0x6006] * iVar1);
                    (&DAT_410ff7a0)[(lVar6 * 0x2002 + (longlong)iVar1) * 3] = 4; // reset age
                }
                lVar4 = lVar4 + 0xd;
                piVar2 = piVar2 + 1;
                lVar3 = lVar3 + -1;
            } while (lVar3 != 0);     // 16 inner (X)
            lVar5 = lVar5 + -1;
        } while (lVar5 != 0);         // 16 outer (Z)
    }
    local_res20 = local_res20 + 0x101;
    local_res10 = local_res10 + 1;
    local_res18 = local_res18 + 0xd08;
} while (local_res10 < 0x40);         // 64 super-chunks
```

What this shows:
- Culling: a super-chunk whose visibility byte (`DAT_40e6f620[superChunk]`) is `0` is skipped completely, or drawn through a prebuilt fallback pass.
- GL calls per chunk: only `glBindBuffer` (index and vertex), `glVertexAttribPointer`, and `glDrawElementsBaseVertex`. No matrix is uploaded per chunk, so the shader probably works out the world position from a chunk origin uniform or from `baseVertex`.
- Index buffer: fixed, bound from `(&DAT_43e98c4c)[LOD]`.
- Vertex buffer: a streamed slot, bound from `(&DAT_43e9908c)[LOD]`.
- Stride: `6` floats (24 bytes), the same at every level.
- Draw count: `indexCount[LOD] * 3` indices, which is `triCount * 3`.
- Base vertex: `baseVertexStep[LOD] * slotIndex`. This shifts the fixed index pattern to the vertices in the uploaded slot.

`Terrain_DrawChunkFromStreamingSlot @ 0xd2c90` is a separate helper that makes exactly the same GL calls (the disassembly is identical). Other passes (shadow or reflection) that walk the same slot table probably use it.

## 3. Marking what is visible

`Terrain_MarkVisibleSectorsAlongCameraRays @ 0x11d470` is a visibility pass on the CPU that marches rays from the camera. It runs before chunks are picked.

### 3.1 The grid of rays

```c
// Terrain_MarkVisibleSectorsAlongCameraRays @ 0x11d470 (excerpt)
local_100 = 0;                         // vertical loop
fVar7 = DAT_002787f4;                  // 48.0
do {
    local_104 = 0;                     // horizontal loop
    fVar31 = (float)local_100 - DAT_002787cc;   // 30.0  => vertical range [-30 .. +29]
    do {
        fVar28 = (float)local_104 - fVar7;     // 48.0  => horizontal range [-48 .. +47]
        ...
        local_104 = local_104 + 1;
    } while ((int)local_104 < 0x60);   // 96 columns
    local_100 = local_100 + 1;
} while (local_100 < 0x3b);            // 60 rows
```

That is 96 × 60 = 5 760 rays.

### 3.2 Normalising the ray directions

```c
fVar22 = SQRT((fVar28*fVar28 + fVar31*fVar31) * fVar3 + fVar5);
// fVar3 = DAT_00278078 = 0.0001085069525288418
// fVar5 = FLOAT_00278280 = 0.25
fVar22 = FLOAT_002783f4 / fVar22;      // 1.0 / length  => normalise
fVar30 = (fVar31 * _DAT_00278130) * fVar22;
// _DAT_00278130 = 0.010416666977107525
fVar29 = fVar22 * _DAT_00278bc0;
// _DAT_00278bc0 = -0.3499999940395355
fVar22 = fVar28 * fVar22 * fVar4;
// fVar4 = DAT_00278110 = 0.0072916666977107525
```

The resulting direction relative to the camera, `(fVar22, fVar30, fVar29)`, is then turned into a world direction with the camera's axes (`camera.rot.forward`, `camera.rot.up`, `camera.rot.right`).

### 3.3 Stepping rays and marking sectors

Each ray takes 48 steps (`iVar12 = 0..0x30`). The step length is `fVar27`:

```c
fVar27 = (float)(local_104 & 7) + DAT_002788c8;   // 248.0 .. 255.0
```

So each step moves the ray about 248–255 world units.

World coordinates at each step are kept as fixed-point integers times 256 (`camera.pos * 256` plus the steps so far). Shifting the integers right gives sector and sub-sector numbers:

| Shift | Unit | Size |
|---|---|---|
| `>> 8` | World unit (1) | none |
| `>> 0xe` (`>> 14`) | Sector number | 64×64×64 world units |
| `>> 0xb` (`>> 11`) | Sub-sector inside the sector | 8×8×8 world units |

Sector lookup table:

```c
iVar1 = *(int *)(DAT_1ada9288 +
    ((iVar17 >> 0xe) + ((iVar11 >> 0xe) * depth + (iVar15 >> 0xe)) * width) * 4);
```

`iVar1 == -1` means empty or outside the map.

Sub-sector visibility array:

```c
lVar8 = (longlong)*(int *)(&DAT_1ada97b0 +
    ((iVar17 >> 0xb & 7) + ((iVar15 >> 0xb & 7) + (iVar11 >> 0xb & 7) * 8) * 8) +
    (longlong)iVar1 * 0x201) * 4);
(&DAT_14167e74)[lVar8 * 0x1b12] = 1;   // mark sub-sector visible
```

### 3.4 Neighbours and portal links

During the first 16 steps (`iVar12 < 0x10`), the code also marks the sectors around the ray, so things don't flicker in and out between frames:

```c
if (iVar12 < 0x10) {
    (&DAT_14167e78)[lVar8 * 0x1b12] = 1;   // extra flag
    if (iVar12 < 2) {
        _raycast_addsector(0, uVar13, uVar10 + 1, uVar14);
        _raycast_addsector(0, uVar13, uVar10 + 2, uVar14);
        _raycast_addsector(0, uVar13, uVar10 + 3, uVar14);
    }
    _raycast_addsector(0, uVar13 - 8, uVar10, uVar14);
    _raycast_addsector(0, uVar13 + 8, uVar10, uVar14);
    _raycast_addsector(0, uVar13, uVar10, uVar14 - 8);
    _raycast_addsector(0, uVar13, uVar10, uVar14 + 8);
}
```

If the sector data has a portal or connection link (`uVar2 & 0xe0000000 == 0xc0000000`), the linked sector is added to the raycast list too, with an offset stored in the low bytes:

```c
if ((uVar2 & 0xe0000000) == 0xc0000000) {
    _raycast_addsector(0, uVar13 - (uVar2 & 0xff),
                       uVar10 - ((int)uVar2 >> 0x10 & 0xff),
                       uVar14 - ((int)uVar2 >> 8 & 0xff));
}
```

The ray stops early when the sub-sector's walk and blocking bits say it is a solid wall (the `0x40000000` pattern) and there is no portal link.

## 4. Asking for terrain height

### 4.1 Collecting triangles in a box

`Terrain_CollectHeightfieldTrianglesAlongSegment @ 0xd05b0` does the main work for every collision and capsule query. It builds a temporary list of triangles in a scratch buffer.

Inputs:
- `param_1`: a scratch `uint` buffer (on the stack, about 73 kB).
- `param_2`: the box's minimum corner (`float*`).
- `param_3`: the box's maximum corner (`float*`).

World to grid

```c
fVar17 = *param_2 - _DAT_41290040;     // worldX - worldOffsetX
fVar18 = *param_3 - _DAT_41290040;     // worldX max
uVar16 = (uint)(param_2[2] - _DAT_41290048); // worldZ min
```

`_DAT_41290040` … `_DAT_41290048` are globals set at runtime, so they can't be read from the file. Other code shows they hold the +4096 world offset (grid = world + 4096), but the exact sign depends on how they are set at runtime.

The function loops over the grid cells in the box. For each active cell (checked against `g_landUseMaskBits`) it adds two triangles to the scratch buffer.

Removing duplicate vertices

```c
// Terrain_CollectHeightfieldTrianglesAlongSegment @ 0xd05b0
uVar11 = *param_1;                      // current vertex count
uVar6  = (ulonglong)(int)uVar11;
uVar14 = 0;
if (0 < (int)uVar11) {
    pfVar9 = (float *)(param_1 + 3);
    do {
        if ((((float)((uint)(fVar20 - pfVar9[-1]) & DAT_00279840) < fVar2) &&
             ((float)((uint)(fVar27 - *pfVar9)      & DAT_00279840) < fVar2) &&
             ((float)((uint)(fVar21 - pfVar9[1])    & DAT_00279840) < fVar2))
            goto LAB_000d0897;          // vertex already exists
        uVar14 = uVar14 + 1;
        pfVar9 = pfVar9 + 4;
    } while ((int)uVar14 < (int)uVar11);
}
if ((int)uVar11 < 0x800) {             // max 2048 unique vertices
    param_1[uVar6 * 4 + 2] = (uint)fVar20;   // X
    param_1[uVar6 * 4 + 3] = (uint)fVar27;   // Y (height)
    param_1[uVar6 * 4 + 4] = (uint)fVar21;   // Z
    *param_1 = *param_1 + 1;
}
```

Two vertices count as the same when they are within `fVar2 = DAT_0027806c = 6.103515625e-05` (about 1/16384). The check takes absolute values with an integer mask, `DAT_00279840 = 0x7fffffff` (clears the sign bit). `DAT_00279870 = 0x80000000` (the sign bit) is used to flip normals.

Adding triangles

Each cell adds two triangles (a split quad). Triangle records start at dword offset `0x2002`:

| Offset (dwords) | Contents |
|---|---|
| `0x2002 + tri*8 + 0` | vertex count (always 3) |
| `+1` | index 0 |
| `+2` | index 1 |
| `+3` | index 2 |
| `+5` | normal X (raw float bits) |
| `+6` | normal Y |
| `+7` | normal Z |

At most 0x3ff = 1023 triangles (`param_1[0x2001]` counts them).

### 4.2 Casting a ray at the heightfield

`RaycastTerrainHeightfieldPatch @ 0xd1830` tests one ray against the terrain. Gameplay raycasts use it (bullets, line of sight, and so on).

It walks the heightfield cells along the ray with a 3D DDA (choosing which axis to step each time):

```c
// RaycastTerrainHeightfieldPatch @ 0xd1830 (excerpt)
do {
    ...
    if ((((uint)((int)uVar9 >> 0xb) < 8) && ((uint)((int)uVar11 >> 0xb) < 8)) &&
        ... landUseMask check ...)
    {
        // fetch the 4 corner heights of the current cell
        lVar7 = (&g_landTileHeightChunks)[lVar10 * 0x684];
        fVar27 = fVar25 + *(float *)(lVar7 + uVar8 * 4);         // (x,z)
        fVar22 = fVar25 + *(float *)(lVar7 + 4 + uVar8 * 4);     // (x+1,z)
        fVar26 = fVar25 + *(float *)(lVar7 + 0x2008 + uVar8 * 4); // (x,z+1)
        fVar25 = fVar25 + *(float *)(lVar7 + 0x2004 + uVar8 * 4); // (x+1,z+1)

        // build two triangles and test with Math_RayIntersectTriangle
        uVar5 = Math_RayIntersectTriangle(..., &local_178, (float *)&local_198, (float *)&local_188);
        if (((int)uVar5 != 0) && (local_1b8[0] < param_1[6])) {
            *param_1    = local_108[0];   // hit point X
            param_1[1]  = local_108[1];   // hit point Y
            param_1[2]  = local_100;       // hit point Z
            param_1[3]  = fVar22;          // normal X
            param_1[4]  = fVar21;          // normal Y
            param_1[5]  = fVar23;          // normal Z
            param_1[6]  = local_1b8[0];    // hit distance (t)
        }
        ... // second triangle of the cell
    }
    // DDA step
    if ((param_1[6] <= fVar28 && fVar28 != param_1[6]) ||
        ((uVar9 == uVar24 && (uVar11 == uVar12)))) break;
    ... // select next axis to step
    iVar15 = iVar15 + 1;
} while (iVar15 <= local_168);   // max 0x4000 steps
```

The result is written to `param_1` as 7 floats:

| Index | Meaning |
|---|---|
| 0..2 | Hit point (X, Y, Z) |
| 3..5 | Face normal (X, Y, Z) |
| 6 | Hit distance `t` along the ray |

The walk stops after at most `0x4000` (16 384) steps, far more than any real ray needs, so in practice there is no limit.

## 5. Terrain collision (capsule or circle against a triangle list)

Collision goes like this:

1. Make a box from the capsule's ends and radius.
2. Collect the triangles in that box into the scratch buffer with `Terrain_CollectHeightfieldTrianglesAlongSegment`.
3. Test the shape against every triangle in the scratch buffer.
4. Also test area geometry (buildings, objects), in case it is hit before the terrain.

### 5.1 Capsule against terrain (people)

`TerrainAndAreaCapsuleIntersectForHumanBody @ 0x33fe0` is the wrapper used by the human simulation (called from `_humansimulation @ 0xbf358`).

```c
// TerrainAndAreaCapsuleIntersectForHumanBody @ 0x33fe0 (excerpt)
fStack_120f8 = *param_1 - param_3;     // AABB min X (capsule start - radius)
fStack_12108 = *param_1 + param_3;     // AABB max X (capsule start + radius)
... // same for Y and Z, then also expanded by param_2 (capsule end)

Terrain_CollectHeightfieldTrianglesAlongSegment(auStack_120b8, &fStack_120f8, &fStack_12108);

uVar3 = CapsuleIntersectTerrainTriangleList(&VStack_120e0.x, (longlong)auStack_120b8,
                                            param_1, param_2, param_3);
if (((int)uVar3 != 0) && (fStack_120c4 < lineIntersectResult.fraction)) {
    lineIntersectResult.pos    = VStack_120e0;
    lineIntersectResult.normal = (fStack_120d4, fStack_120d0, fStack_120cc);
    lineIntersectResult.fraction = fStack_120c4;
    lineIntersectResult.areaId   = 0;
}

// then test against 4 streamed area objects
for (uVar4 = 0; uVar4 < 4; ++uVar4) {
    if (areaPtr[uVar4] != 0) {
        iVar1 = _capsuleintersectarea(..., uVar4, param_1, param_2, param_3);
        ...
    }
}
```

`CapsuleIntersectTerrainTriangleList @ 0x337b0` goes through the triangles in the scratch buffer:

```c
// CapsuleIntersectTerrainTriangleList @ 0x337b0 (excerpt)
if (0 < *(int *)(param_2 + 0x8004)) {      // triangle count at dword 0x2001
    piVar4 = (int *)(param_2 + 0x8010);      // first triangle record
    do {
        // fetch 3 vertices from the deduped vertex table
        lVar2 = (longlong)piVar4[-1];        // index0
        local_c8 = *(float *)(param_2 + 8 + lVar2 * 0x10);   // vertex X
        local_c4 = *(float *)(param_2 + 0xc + lVar2 * 0x10); // vertex Y
        local_c0 = *(float *)(param_2 + 0x10 + lVar2 * 0x10);// vertex Z
        ... // index1 -> local_b8/b4/b0, index2 -> local_d8/d4/d0

        // compute face normal
        local_f8 = (local_d4 - local_c4)*(local_b0 - local_c0) - ...;
        fStack_f4 = ...;
        local_f0 = ...;
        fVar6 = SQRT(local_f8*local_f8 + fStack_f4*fStack_f4 + local_f0*local_f0);
        if (fVar6 == 0.0) { local_f8 = 0; fStack_f4 = 0; local_f0 = 0; }
        else { fVar6 = 1.0 / fVar6; local_f8 *= fVar6; ... }

        // test swept capsule against this triangle
        uVar3 = _capsuleintersecttri(&local_e8, &local_f8, local_res8,
                                     param_3, param_4, param_5,
                                     &local_c8, &local_b8, &local_d8);
        if (((int)uVar3 != 0) && (local_res8[0] < param_1[7])) {
            *param_1 = local_e8; ... param_1[7] = local_res8[0]; // keep closest hit
        }

        // if the triangle record indicates a quad split (piVar4[-2] == 4), test the 2nd tri
        if (piVar4[-2] == 4) {
            lVar2 = (longlong)piVar4[2];     // index3
            ...
            uVar3 = _capsuleintersecttri(..., &local_c8, &local_d8, &local_a8);
            ...
        }
        iVar5 = iVar5 + 1;
        piVar4 = piVar4 + 8;                  // next triangle record (8 dwords)
    } while (iVar5 < *(int *)(param_2 + 0x8004));
}
```

If it finds a hit closer than `DAT_00278adc = 65536.0`, the function returns `1`.

### 5.2 Circle or sphere against terrain (wheels and rigid bodies)

`TerrainAndAreaCapsuleIntersectForWheelOrRigidBody @ 0x3de80` is the vehicle version. It is built the same way as the human wrapper, but calls `CircleIntersectTerrainTriangleList @ 0x3dae0` instead of the capsule version.

`CircleIntersectTerrainTriangleList` goes through the same scratch-buffer triangles but calls `_circleintersecttriangle`. Only the shape test differs; fetching triangles, working out normals, and keeping the closest hit are the same.

### 5.3 Also testing area geometry

Both wrappers loop over 4 streamed area objects (`uVar4 = 0..3`). The area geometry is at `DAT_1ada9240 + uVar4 * 0x4035c`. If an area object is active (`*piVar5 != 0`), the wrapper tests the same capsule or circle against that area's collision geometry and updates `lineIntersectResult` if the area is hit before the terrain.

## 6. Constants

| Address | Raw (LE) | Value | Meaning or use |
|---|---|---|---|
| `0x278814` | `00 00 80 42` | 64.0 | Half a cell (128 / 2), used to find the chunk centre |
| `0x278a1c` | `00 00 80 44` | 1024.0 | Half a super-chunk (128 cells × 8 world units / 2?) |
| `0x278aec` | `00 00 80 48` | 262 144.0 | LOD0 squared distance limit (512²) |
| `0x278af4` | `00 00 10 49` | 589 824.0 | LOD1 limit (about 768²) |
| `0x278afc` | `00 00 80 49` | 1 048 576.0 | LOD2 limit (1024²) |
| `0x278b00` | `00 00 10 4a` | 2 359 296.0 | LOD3 limit (about 1536²) |
| `0x278b04` | `00 00 80 4a` | 4 194 304.0 | LOD4 limit (2048²) |
| `0x278b08` | `00 00 10 4b` | 9 437 184.0 | LOD5 limit (about 3072²) |
| `0x278b0c` | `00 00 80 4b` | 16 777 216.0 | LOD6, the furthest you can see (4096²) |
| `0x27806c` | `00 00 80 38` | 6.1035156e-05 | How close two vertices must be to count as one in `CollectHeightfieldTriangles` |
| `0x2783f4` | `00 00 80 3f` | 1.0 | 1.0, used everywhere for 1.0 / length |
| `0x278280` | `00 00 80 3e` | 0.25 | Ray projection constant for sector marking |
| `0x278110` | `ef ee ee 3b` | 0.0072916667 | Horizontal field-of-view scale in the ray grid |
| `0x278078` | `3a 8e e3 38` | 0.00010850695 | Combined field of view and aspect in the ray grid |
| `0x278130` | `ab aa 2a 3c` | 0.010416667 | Vertical field-of-view scale in the ray grid |
| `0x278bc0` | `33 33 b3 be` | -0.35 | Camera-space Z offset in the ray grid |
| `0x278388` | `33 33 33 3f` | 0.7 | Unknown ray grid value |
| `0x2787f4` | `00 00 40 42` | 48.0 | Half the ray grid's width |
| `0x2787cc` | `00 00 f0 41` | 30.0 | Half the ray grid's height |
| `0x2788d8` | `00 00 80 43` | 256.0 | World to fixed-point scale (used when marching rays) |
| `0x2788c8` | `00 00 78 43` | 248.0 | Base ray step (plus 0..7 of jitter) |
| `0x278774` | `00 00 80 41` | 16.0 | Height falloff constant in `Terrain_RebuildChunkSlot` |
| `0x278adc` | `00 00 80 47` | 65 536.0 | Collision distance limit (furthest valid hit) |
| `0x279840` | `ff ff ff 7f` | 0x7fffffff | Integer mask that clears the sign bit, for absolute values |
| `0x279870` | `00 00 00 80` | 0x80000000 | Sign-bit mask used to flip normals |

## 7. Unknowns

1. Where LOD6's buffer lives. There are seven blocks of detail-level data (0..6), and the draw loop uses up to level 6, but `Render_InitTerrainStreamingBuffers` only sets up six main GL banks. Static analysis could not tell whether LOD6 reuses one of those banks, uses the extra 512 KiB shared vertex buffer, or is drawn some other way.

2. The world-offset globals set at runtime. `DAT_41290040` … `DAT_41290048` hold the world-to-grid offset (thought to be `+4096` from other evidence). They are in `.bss` and can't be read from the file; their values depend on how the map is set up.

3. Sector and sub-sector layout. The visibility arrays at `DAT_14167e74`, `DAT_14161248`, `DAT_14164248` and the sector lookup tables at `DAT_1ada9288` / `DAT_1ada97b0` are large structures with no starting data. Their field sizes, bit layouts, and what the portal-link encoding means (`0xc0000000` and so on) were guessed from how they are used, not checked against a real struct definition.

4. The real values of `g_terrainChunkSlotsLod0Count`. The slot count for each level is in globals in the `.bss` segment, which has no starting data. Their values (0x80, 0x40, or something else) can't be read from the file; only how they are used (masking with `& count-1`, so a power of two) was seen.

5. How the capsule and circle tests work inside. The low-level tests `_capsuleintersecttri @ 0x35690` and `_circleintersecttriangle` were outside the functions assigned, so they were treated as black boxes. Their exact swept-sphere or Minkowski-sum methods are not described here.

6. `RenderLandscapeChunkIndexBufferPass`. The fallback path (`local_res20[-1] == 0`) calls this function, which probably draws prebuilt or merged index buffers for far or hidden super-chunks. It was outside the functions assigned and is not covered.
