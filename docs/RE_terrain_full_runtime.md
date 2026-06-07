# Sub Rosa Terrain Runtime / Render / Query Pipeline

**Target:** `subrosa.exe.THIS_ONE` (image base 0, x86-64 LE)  
**Scope:** How terrain is streamed, drawn, culled, queried, and collided with at runtime.  
**Status:** Read-only reverse engineering report. All claims cite exact Ghidra addresses and decompile snippets.

---

## 1. Streaming Model

### 1.1 World subdivision

The runtime terrain is subdivided into **64 super-chunks** arranged in an 8×8 grid. Each super-chunk is further split into a **16×16 grid of sub-chunks** (256 total). That gives **64 × 256 = 16 384** addressable chunks in the world.

- Chunk metadata (world origin, tile coords, etc.) lives in a per-super-chunk struct of **0xd08 (3336) bytes** indexed by the super-chunk id.
- Inside `Terrain_SelectAndRefreshVisibleChunkSlotsAroundPoint @ 0xd26a0`, the outer loop runs `0..63` over the super-chunks. For each super-chunk it iterates a 16×16 sub-chunk grid (`0..15` inner, `0..15` outer).

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

### 1.2 LOD distance thresholds

`Terrain_SelectAndRefreshVisibleChunkSlotsAroundPoint` computes the squared distance from the supplied camera point to each sub-chunk centre and selects one of **7 LOD levels** (0 = highest detail, 6 = lowest).

The thresholds are read from `.rdata` as a contiguous array of IEEE-754 floats:

| Address | Raw LE | Decoded float | Meaning |
|---|---|---|---|
| `0x278aec` | `00 00 80 48` | **262 144.0** | LOD0  cutoff  (< 512 world units) |
| `0x278af4` | `00 00 10 49` | **589 824.0** | LOD1  cutoff  (< ~768) |
| `0x278afc` | `00 00 80 49` | **1 048 576.0** | LOD2  cutoff  (< 1024) |
| `0x278b00` | `00 00 10 4a` | **2 359 296.0** | LOD3  cutoff  (< ~1536) |
| `0x278b04` | `00 00 80 4a` | **4 194 304.0** | LOD4  cutoff  (< 2048) |
| `0x278b08` | `00 00 10 4b` | **9 437 184.0** | LOD5  cutoff  (< ~3072) |
| `0x278b0c` | `00 00 80 4b` | **16 777 216.0** | LOD6 / max view radius (< 4096) |

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

If the desired LOD’s slot pool is exhausted, the code falls back to the **next coarser LOD** until it finds a free slot (`do { ... uVar10 = uVar10 + 1; } while (...)`).

### 1.3 Slot allocation and eviction

Each LOD maintains its own circular slot pool.

- Slot metadata base for LOD *n* is at `0x410ff780 + n * 0x18018`.
- The pool entry size is 12 bytes (3 dwords): chunk id, sub-chunk id, age.
- `g_terrainChunkSlotsLod0Count` (and the 6 siblings spaced `0x6006` dwords apart) stores the per-LOD slot count.
- `DAT_410ff794 + n * 0x18018` stores the last-used slot index (LRU cursor).

`Terrain_RebuildChunkSlot @ 0xd0c10` chooses a slot like this:

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

The age is set to **4** when a slot is freshly rebuilt. In `Terrain_SelectAndRefreshVisibleChunkSlotsAroundPoint`, every frame all ages are decremented by 1 if `> 0`, and slots with `age == 0` are considered recyclable.

### 1.4 Vertex generation per slot

`Terrain_RebuildChunkSlot` walks a regular grid over the 128×128 cell chunk with a step of `1 << LOD`:

```c
iVar21 = 1 << ((byte)param_3 & 0x1f);   // grid step = 2^LOD
iVar4  = iVar21 - 1;                    // step-1
```

Loop bounds are `-1 .. 0x82` (0..129 effective), so vertex counts are:

| LOD | Step | Vertices / side | Total verts |
|---|---|---|---|
| 0 | 1 | 129 | 16 641 |
| 1 | 2 | 65 | 4 225 |
| 2 | 4 | 33 | 1 089 |
| 3 | 8 | 17 | 289 |
| 4 | 16 | 9 | 81 |
| 5 | 32 | 5 | 25 |
| 6 | 64 | 3 | 9 |

For each grid vertex it samples the **global 2049×2049 heightfield** via `g_landTileHeightChunks[superChunk * 0x684]`, computes the normal by differencing neighbouring heights, and writes an interleaved **position + normal** vertex to a CPU scratch buffer at `DAT_40e7f720`.

Vertex format (confirmed by `glVertexAttribPointer` calls):

- **Attrib 0** (position): 3× `GL_FLOAT`, stride = 24 bytes, offset = 0
- **Attrib 1** (normal):   3× `GL_FLOAT`, stride = 24 bytes, offset = 12

The completed vertex buffer is uploaded with `glBufferSubData`:

```c
(*glBufferSubDataProc)(GL_ARRAY_BUFFER,
    (longlong)(int)((&g_terrainChunkSlotsLod0IndexBufferStride)[lVar18 * 0x6006] * uVar32),
    (longlong)(local_f8 * 6 * 4),               // vertexCount * 6 floats * 4 bytes
    &DAT_40e7f720);
```

`local_f8` is the total vertex count (e.g. 16641 for LOD0).

### 1.5 GL buffer banks (streaming buffers)

`Render_InitTerrainStreamingBuffers @ 0xc2db0` creates **six** primary cache banks plus one shared vertex buffer:

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

Summary:

| Bank | GL Target | Slots | Slot size | Total size | Purpose |
|---|---|---|---|---|---|
| 0 | `GL_ARRAY_BUFFER`   | 0x80 | 0x80000 | 64 MiB | Vertex data |
| 1 | `GL_ARRAY_BUFFER`   | 0x80 | 0x80000 | 64 MiB | Vertex data |
| 2 | `GL_ELEMENT_ARRAY_BUFFER` | 0x40 | 0x80000 | 32 MiB | Index data |
| 3 | `GL_ELEMENT_ARRAY_BUFFER` | 0x40 | 0x80000 | 32 MiB | Index data |
| 4 | `GL_ELEMENT_ARRAY_BUFFER` | 0x40 | 0x80000 | 32 MiB | Index data |
| 5 | `GL_ELEMENT_ARRAY_BUFFER` | 0x40 | 0x80000 | 32 MiB | Index data |
| — | `GL_ARRAY_BUFFER` (extra) | 1 | 0x80000 | 512 KiB | Shared terrain streaming buffer |

The **index buffers are static/precomputed**; the code never calls `glBufferSubData` on them at runtime. Only vertex data is streamed per chunk. Each slot in the vertex buffer is a fixed 0x80000-byte region. The actual vertex data is smaller (e.g. 399 kB for LOD0) and the remainder is padding.

`glDrawElementsBaseVertex` is used so that every chunk slot shares the **same index pattern** but offsets the vertex indices by `baseVertexStep * slotIndex`.

**Unresolved:** the draw loop (`RenderLandscapeVisibleChunkPasses`) references **7 LOD levels** (0..6), yet only **6 primary banks** are initialised here. The exact mapping of LOD6 to one of the existing banks, or to the extra shared buffer, could not be fully determined from static analysis alone.

---

## 2. Per-Chunk Draw Path

The terrain draw orchestrator is **`RenderLandscapeVisibleChunkPasses @ 0xd22e0`**, called from `RenderWorld @ 0x11fd00`.

### 2.1 Shader / texture setup (first pass only)

When `param_1 == 0` (the colour / main pass):

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

All passes bind the landscape mask texture on **texture unit 3** (`GL_TEXTURE3`).

### 2.2 Chunk draw loop

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

Key facts:
- **Culling:** a super-chunk whose visibility byte (`DAT_40e6f620[superChunk]`) is `0` is skipped entirely (or drawn via a fallback prebuilt pass).
- **GL state per chunk:** only `glBindBuffer` (index + vertex), `glVertexAttribPointer`, and `glDrawElementsBaseVertex` are issued. No matrix uploads per chunk; the shader likely reconstructs world position from the chunk origin uniform or from `baseVertex`.
- **Index buffer:** static, bound from `(&DAT_43e98c4c)[LOD]`.
- **Vertex buffer:** streamed slot, bound from `(&DAT_43e9908c)[LOD]`.
- **Stride:** `6` floats (24 bytes), same for all LODs.
- **Draw count:** `indexCount[LOD] * 3` indices = `triCount * 3`.
- **Base vertex:** `baseVertexStep[LOD] * slotIndex`. This offsets the static index pattern to point at the uploaded slot vertices.

`Terrain_DrawChunkFromStreamingSlot @ 0xd2c90` is a standalone helper that performs the exact same GL sequence (confirmed by identical disassembly). It is likely used by secondary passes (shadow / reflection) that iterate over the same slot table.

---

## 3. Visibility / Sector Marking Algorithm

`Terrain_MarkVisibleSectorsAlongCameraRays @ 0x11d470` is a CPU-side ray-marching visibility pass that runs **before** chunk selection.

### 3.1 Ray grid

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

Total rays cast: **96 × 60 = 5 760**.

### 3.2 Ray direction normalisation

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

The resulting camera-space direction `(fVar22, fVar30, fVar29)` is then transformed into world space by the camera basis vectors (`camera.rot.forward`, `camera.rot.up`, `camera.rot.right`).

### 3.3 Ray stepping and sector marking

Each ray steps **48 times** (`iVar12 = 0..0x30`). The step length is `fVar27`:

```c
fVar27 = (float)(local_104 & 7) + DAT_002788c8;   // 248.0 .. 255.0
```

So each step advances the ray by roughly **248–255 world units**.

World coordinates at each step are kept as fixed-point integers scaled by 256 (`camera.pos * 256` plus accumulated step). The integer coordinates are right-shifted to obtain sector / sub-sector indices:

| Shift | Resulting unit | Size |
|---|---|---|
| `>> 8` | World unit (1) | — |
| `>> 0xe` (`>> 14`) | Sector index | **64×64×64** world units |
| `>> 0xb` (`>> 11`) | Sub-sector index within sector | **8×8×8** world units |

Sector lookup table:

```c
iVar1 = *(int *)(DAT_1ada9288 +
    ((iVar17 >> 0xe) + ((iVar11 >> 0xe) * depth + (iVar15 >> 0xe)) * width) * 4);
```

`iVar1 == -1` means empty / out of bounds.

Sub-sector visibility array:

```c
lVar8 = (longlong)*(int *)(&DAT_1ada97b0 +
    ((iVar17 >> 0xb & 7) + ((iVar15 >> 0xb & 7) + (iVar11 >> 0xb & 7) * 8) * 8) +
    (longlong)iVar1 * 0x201) * 4);
(&DAT_14167e74)[lVar8 * 0x1b12] = 1;   // mark sub-sector visible
```

### 3.4 Neighbour expansion and portal links

During the first 16 steps (`iVar12 < 0x10`), the code marks **neighbouring sectors** to combat temporal aliasing:

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

If the sector metadata contains portal / connectivity links (`uVar2 & 0xe0000000 == 0xc0000000`), the linked sector is also added to the raycast list with an offset encoded in the low bytes:

```c
if ((uVar2 & 0xe0000000) == 0xc0000000) {
    _raycast_addsector(0, uVar13 - (uVar2 & 0xff),
                       uVar10 - ((int)uVar2 >> 0x10 & 0xff),
                       uVar14 - ((int)uVar2 >> 8 & 0xff));
}
```

The ray terminates early if the sub-sector’s walkability / occlusion bits indicate a solid wall (`0x40000000` pattern) and no portal link is present.

---

## 4. Terrain Height-Query Path

### 4.1 Collecting triangles along a segment / AABB

`Terrain_CollectHeightfieldTrianglesAlongSegment @ 0xd05b0` is the workhorse for all collision and capsule queries. It builds a temporary triangle list in a scratch buffer.

**Inputs:**
- `param_1` — scratch `uint` buffer (stack-allocated, ~73 kB).
- `param_2` — AABB minimum (`float*`).
- `param_3` — AABB maximum (`float*`).

**World → grid mapping**

```c
fVar17 = *param_2 - _DAT_41290040;     // worldX - worldOffsetX
fVar18 = *param_3 - _DAT_41290040;     // worldX max
uVar16 = (uint)(param_2[2] - _DAT_41290048); // worldZ min
```

`_DAT_41290040` … `_DAT_41290048` are runtime-initialised globals (not readable in the static image). From the broader context they are known to encode the **+4096 world offset** (grid = world + 4096), but the exact sign depends on runtime initialisation.

The function loops over integer grid cells in the AABB span and, for each active cell (checked against `g_landUseMaskBits`), emits two triangles into the scratch buffer.

**Vertex deduplication**

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

The dedup tolerance is `fVar2 = DAT_0027806c = 6.103515625e-05` (~1/16384) with an integer masking trick using `DAT_00279840 = 0x7fffffff` (clear sign bit) and `DAT_00279870 = 0x80000000` (sign bit mask used to flip normals).

**Triangle emission**

Each cell contributes two triangles (quad split). Triangle records are stored starting at dword offset `0x2002`:

| Offset (dwords) | Content |
|---|---|
| `0x2002 + tri*8 + 0` | vertex count (always 3) |
| `+1` | index 0 |
| `+2` | index 1 |
| `+3` | index 2 |
| `+5` | normal X (raw float bits) |
| `+6` | normal Y |
| `+7` | normal Z |

Maximum triangles: **0x3ff = 1023** (`param_1[0x2001]` is the triangle counter).

### 4.2 Raycast against the heightfield

`RaycastTerrainHeightfieldPatch @ 0xd1830` performs a **single-ray vs terrain** test used by gameplay raycasts (e.g. bullets, line-of-sight).

It implements a **3D DDA grid traversal** (integer step selection) over the heightfield cells:

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

The ray result is written to `param_1` as a 7-float record:

| Index | Meaning |
|---|---|
| 0..2 | Hit point (X, Y, Z) |
| 3..5 | Face normal (X, Y, Z) |
| 6 | Hit distance `t` along the ray |

The traversal limit is `0x4000` (16 384) steps, which is far larger than any practical ray, effectively unbounded.

---

## 5. Terrain Collision (Capsule / Circle vs Triangle List)

The collision pipeline is:

1. **Build AABB** from capsule endpoints + radius.
2. **Collect triangles** into scratch buffer via `Terrain_CollectHeightfieldTrianglesAlongSegment`.
3. **Test primitive** against every triangle in the scratch buffer.
4. **Fallback** to area geometry (buildings, objects) if terrain hit is farther.

### 5.1 Capsule vs terrain (human body)

`TerrainAndAreaCapsuleIntersectForHumanBody @ 0x33fe0` is the wrapper used by the human simulation (`_humansimulation @ 0xbf358` caller).

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

`CapsuleIntersectTerrainTriangleList @ 0x337b0` iterates the scratch-buffer triangles:

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

If a hit is found within distance `DAT_00278adc = 65536.0`, the function returns `1`.

### 5.2 Circle / sphere vs terrain (wheels / rigid bodies)

`TerrainAndAreaCapsuleIntersectForWheelOrRigidBody @ 0x3de80` is the vehicle path. It is structurally identical to the human wrapper but calls **`CircleIntersectTerrainTriangleList @ 0x3dae0`** instead of the capsule version.

`CircleIntersectTerrainTriangleList` iterates the same scratch-buffer triangles but calls `_circleintersecttriangle`. The only difference is the primitive test function; the triangle-fetch loop, normal computation, and closest-hit tracking are identical.

### 5.3 Area geometry fallback

Both wrappers loop over **4 streamed area objects** (`uVar4 = 0..3`). The area geometry is stored at `DAT_1ada9240 + uVar4 * 0x4035c`. If an area object is active (`*piVar5 != 0`), the wrapper tests the same capsule/circle against that area’s collision geometry and updates `lineIntersectResult` if the area hit is closer than the terrain hit.

---

## 6. Constants Table

| Address | Raw (LE) | Decoded | Meaning / Usage |
|---|---|---|---|
| `0x278814` | `00 00 80 42` | **64.0** | Half-cell size (128 / 2) used in chunk centre offset |
| `0x278a1c` | `00 00 80 44` | **1024.0** | Half super-chunk size (128 cells × 8 world units / 2?) |
| `0x278aec` | `00 00 80 48` | **262 144.0** | LOD0 squared distance threshold (512²) |
| `0x278af4` | `00 00 10 49` | **589 824.0** | LOD1 threshold (~768²) |
| `0x278afc` | `00 00 80 49` | **1 048 576.0** | LOD2 threshold (1024²) |
| `0x278b00` | `00 00 10 4a` | **2 359 296.0** | LOD3 threshold (~1536²) |
| `0x278b04` | `00 00 80 4a` | **4 194 304.0** | LOD4 threshold (2048²) |
| `0x278b08` | `00 00 10 4b` | **9 437 184.0** | LOD5 threshold (~3072²) |
| `0x278b0c` | `00 00 80 4b` | **16 777 216.0** | LOD6 / max view radius (4096²) |
| `0x27806c` | `00 00 80 38` | **6.1035156e-05** | Vertex dedup epsilon in `CollectHeightfieldTriangles` |
| `0x2783f4` | `00 00 80 3f` | **1.0** | Normalisation scalar (1.0 / length) used everywhere |
| `0x278280` | `00 00 80 3e` | **0.25** | Ray projection constant in sector marking |
| `0x278110` | `ef ee ee 3b` | **0.0072916667** | Horizontal FOV scale in ray grid |
| `0x278078` | `3a 8e e3 38` | **0.00010850695** | Combined FOV / aspect term in ray grid |
| `0x278130` | `ab aa 2a 3c` | **0.010416667** | Vertical FOV scale in ray grid |
| `0x278bc0` | `33 33 b3 be` | **-0.35** | Camera-space Z component bias in ray grid |
| `0x278388` | `33 33 33 3f` | **0.7** | Unknown ray grid scalar |
| `0x2787f4` | `00 00 40 42` | **48.0** | Horizontal ray grid half-width |
| `0x2787cc` | `00 00 f0 41` | **30.0** | Vertical ray grid half-height |
| `0x2788d8` | `00 00 80 43` | **256.0** | World-to-fixed-point scale (used in ray marching) |
| `0x2788c8` | `00 00 78 43` | **248.0** | Base ray step length (plus 0..7 jitter) |
| `0x278774` | `00 00 80 41` | **16.0** | Height falloff constant in `Terrain_RebuildChunkSlot` |
| `0x278adc` | `00 00 80 47` | **65 536.0** | Collision hit distance threshold (max valid hit) |
| `0x279840` | `ff ff ff 7f` | **0x7fffffff** | Integer sign-bit mask for absolute-value trick |
| `0x279870` | `00 00 00 80` | **0x80000000** | Sign-bit mask used to flip normals |

---

## 7. Honest Unknowns

1. **LOD6 buffer mapping.** Seven LOD metadata blocks exist (0..6) and the draw loop references up to index 6, but `Render_InitTerrainStreamingBuffers` only initialises six primary GL banks. Whether LOD6 re-uses one of the existing banks, uses the extra 512 KiB shared vertex buffer, or is handled by a separate render path could not be resolved statically.

2. **Runtime world-offset globals.** `DAT_41290040` … `DAT_41290048` encode the world→grid offset (believed to be `+4096` based on external context). They reside in the `.bss` region and cannot be read from the static image; their exact runtime values depend on map initialisation.

3. **Sector / sub-sector struct layout.** The visibility arrays at `DAT_14167e74`, `DAT_14161248`, `DAT_14164248`, and the sector lookup tables at `DAT_1ada9288` / `DAT_1ada97b0` are large, uninitialised structures. Their exact field sizes, bit layouts, and the meaning of the portal-link encoding (`0xc0000000` etc.) were inferred from usage patterns but not verified against a concrete struct definition.

4. **`g_terrainChunkSlotsLod0Count` actual values.** The per-LOD slot counts are stored in globals that lie in the uninitialised `.bss` segment. Their values (e.g. 0x80, 0x40, or other numbers) are not readable statically; only their usage pattern (power-of-two masking `& count-1`) was observed.

5. **Capsule/circle intersection internals.** The low-level primitive tests `_capsuleintersecttri @ 0x35690` and `_circleintersecttriangle` are **outside the assigned function set** and were treated as black boxes. Their exact Minkowski-sum or swept-sphere algorithms are not documented here.

6. **`RenderLandscapeChunkIndexBufferPass`.** The fallback path (`local_res20[-1] == 0`) calls this function, which presumably draws prebuilt or merged index buffers for distant / invisible super-chunks. Its internals were not part of the assigned set and are not covered.

---

*End of report.*
