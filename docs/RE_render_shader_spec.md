# Sub Rosa render and shader specification

Status: read-only reverse-engineering notes. The shader bodies referenced below
are the captured stock sources in `../../client/src/shaders/stock`; they are a
stronger source of truth than a new approximation in the map editor.

## Native program set

The Windows binary stores the shader source strings at these addresses. The
renderer creates the programs in `renderInitShadersAndWriteReport` and keeps
them in `shaderInfos`.

| Program | Vertex | Fragment | Geometry | Use |
| --- | ---: | ---: | ---: | --- |
| 0 | `0x2f61e0` | `0x2f6460` | — | world objects and blocks |
| 1 | `0x2f7bb0` | `0x2f7e60` | — | array-world objects |
| 2 | `0x2f9820` | `0x2f9a10` | — | reflective objects |
| 3 | `0x2f9fd0` | `0x2fa480` | — | models |
| 5 | `0x2fc070` | `0x2fc310` | — | depth prepass |
| 6 | `0x2fc620` | `0x2fc780` | — | sky |
| 8 | `0x2fe670` | `0x2fe9a0` | — | landscape |
| 9 | `0x2ff460` | `0x2ff4e0` | `0x2ff550` | shadow volumes |
| 10 | `0x3006e0` | `0x300830` | — | cubemap blur |
| 11 | `0x300dd0` | `0x300ec0` | — | HDR composite |
| 12 | `0x3012a0` | `0x301390` | — | water |

The exact captured sources are in the sibling client repository:

- `../../client/src/shaders/stock/world_vertex.hpp`
- `../../client/src/shaders/stock/world_fragment.hpp`
- `../../client/src/shaders/stock/landscape_vertex.hpp`
- `../../client/src/shaders/stock/landscape_fragment.hpp`
- `../../client/src/shaders/stock/sky_vertex.hpp`
- `../../client/src/shaders/stock/sky_fragment.hpp`
- `../../client/src/shaders/stock/shadow_volume_vertex.hpp`
- `../../client/src/shaders/stock/shadow_volume_geometry.hpp`
- `../../client/src/shaders/stock/shadow_volume_fragment.hpp`

The source addresses and uniform setup are recovered in
`../../git-recovered-blobs/blob_9.txt`. Linux runtime function addresses are in
`../../client/src/offsets.hpp`.

## World material and shadow contract

The world vertex shader takes position, a three-component texture coordinate,
and normal at locations 0, 1, and 2. It passes world position, normal, light
vector, and view vector to the fragment stage.

The fragment stage uses these texture units:

| Unit | Uniform | Meaning |
| ---: | --- | --- |
| 0 | `tex_color` | base color, sRGB |
| 1 | `tex_shadowmap` | screen-space shadow factor |
| 2 | `tex_material` | packed material map, sRGB in the stock source |
| 3 | `tex_cubemap` | environment reflection |
| 4 | `tex_atlas_color` | array overlay color |
| 5 | `tex_atlas_material` | array overlay material |

It converts the base color and material map to linear space, uses material R
as metallic and G as roughness, evaluates GGX/Fresnel, multiplies direct light
by the sampled shadow factor, adds cubemap diffuse and specular environment
terms, then converts the result to sRGB. The shadow factor is sampled as
`texture(tex_shadowmap, gl_FragCoord.xy * ooresolution).r`; it is not a
vertex-color bake and it is not a stencil value.

The native shadow volume program uses adjacency geometry. The geometry shader
receives `triangles_adjacency`, identifies back-facing faces using
`dot(faceNormal, lightdirection)`, extrudes them by
`-lightdirection * shadowlength`, emits silhouette side quads only where the
adjacent face faces the light, and emits front/back caps. Its fragment shader
writes `vec4(0, 1, 0, 0.0625)`. This accumulated buffer becomes the red shadow
factor consumed by the world and model shaders.

## Landscape contract

The landscape vertex shader computes these coordinates from world position:

```text
color.s = (x / 8 + 512) / 2048
color.t = 1 - (z / 8 + 512) / 2048
detail.s = x / 32
detail.t = 1 - z / 32
mask.s = (x / 8 + 512) / 2048
mask.t = (z / 8 + 512) / 2048
```

It also selects a tire-mark layer from `floor(position.xz / 128) & 15`, giving
256 wrapped layers. The fragment stage uses `tex_normal` unit 0,
`tex_detail` unit 1, `tex_color` unit 2, `tex_mask` (unsigned integer) unit 3,
and `tex_shadow` unit 4. If `land_usemask == 1`, it extracts one bit from the
16-bit mask texel and discards the fragment when that bit is clear.

The native terrain lighting is not the world PBR path. It uses `rho = 3` and
`k = 4/3`, then computes a wrapped diffuse scale from the normal/light dot
product. It multiplies linear detail and color textures by:

```text
lightambient * (0.375 + dot(normal, light) * 0.125)
+ lightdiffuse * scale * shadow * 0.625
```

It adds a detail-dependent view shine term, applies the terrain tire mark, and
converts to sRGB. Terrain therefore needs its own shader and all five native
texture inputs. Reusing a generic colored mesh shader changes both the texture
mapping and the lighting response.

## Sky contract

The sky program uses `tex_depth` unit 0, `atmoconstant`, `atmowavelength`,
`ooresolution`, `worldposition`, `lightdirection`, and `numofsamples = 4`.
The default atmosphere arrays are at Windows `0x2f61b8` and `0x2f61c8`; Linux
offsets are `0x3889f0` and `0x3889e0`. The source performs depth-aware
atmospheric scattering and only composites clouds for pixels whose packed depth
is greater than 65536. The cloud mix is supplied by
`SRC_CLOUD_SHADER_SOURCE` in the client build. A sky shader without this mix is
not the complete game sky.

## Native pass order and entry points

The relevant Windows entry points are:

| Address | Function |
| ---: | --- |
| `0x11fd00` | `renderWorld` |
| `0x122690` | `renderSky` |
| `0x946d0` | `renderShadows` |
| `0x93e60` | landscape cubemap capture and blur |

Linux equivalents are `0x127844`, `0x54022`, `0xc49cb`, and `0x8bab5`.
The depth prepass encodes view distance into RGB. The sky reads that packed
depth to distinguish geometry from the far sky. Shadow volumes are rendered
into the shadow accumulation target before world and landscape color passes.
Landscape chunks then sample the same screen-space shadow result as world
objects.

## Current editor mismatches

These are concrete, not stylistic:

1. `src/render_gpu.cpp` currently defines new GLSL strings instead of reusing
   the captured stock landscape, world, sky, and shadow-volume sources. Its sky
   code has no cloud source and its terrain path has no native normal texture or
   unsigned land-use mask.
2. The editor's `LitShader` uses a custom `atmosphereWorldPosition` and directly
   evaluates an atmosphere function in the world shader. Native world objects
   sample the cubemap for environment lighting; the sky program owns the
   depth-aware atmosphere pass.
3. The editor uploads road/terrain runs as generic mesh materials. To match the
   game, the road/terrain renderer must provide native landscape texture UVs,
   `tex_detail`, `tex_color`, `tex_shadow`, and mask inputs, then use the native
   landscape lighting equation.
## 2026-09-17 source corrections

Rechecked `/subrosa.exe.THIS_ONE` through Ghidra:

- `RenderSky @ 0x122690` disables depth/culling and uses `GL_ONE, GL_SRC_ALPHA`.
- `_glstateshadowtwoside @ 0xa6fb0` counts back/front faces with opposite wrapping stencil operations and a reversed depth comparison. The editor uses closed volumes with depth-fail counting under its normal depth comparison.
- `RenderQueuedShadowVolumeAdjacencyPass @ 0x11c950` scales building extrusion by height and `8 / max(light.y, 0.125)`. The constants at `0x278728` and `0x278224` are `8.0` and `0.125`.
- The queued district pass at `0x946d0` uploads `32.0` from `0x2787d4` as its extrusion length.
- Captured `stock/world_fragment.hpp` multiplies direct diffuse and specular by the red shadow factor before adding environment lighting.

The editor now writes packed depth on desktop as well as WebGL, explicitly enables its attachment when clearing it, and guards zero atmospheric density. It caches triangle adjacency for buildings and district meshes, closes silhouette sides with consistent winding, and uses height-dependent building extrusion. Direct lighting is stored separately and removed at shadowed pixels before tone mapping; ambient light, reflections and sky are retained. Gizmos are drawn after the final resolve.

This is source-level alignment, not a claim of image parity. The cubemap, cloud and landscape differences above remain. No build or runtime comparison was performed for this change, as requested.

## Source-of-truth rule

Do not tune constants in the editor shader until the captured stock source and
the matching render pass have been checked. If a texture is unavailable, keep
the native sampler and provide an explicit fallback texture with the same
space, channel order, wrap, and filtering. Do not silently substitute a
vertex-color approximation for a shadow map or terrain material map.

## 2026-09-17 lighting and shadow-composite findings (Ghidra, subrosa.exe)

- `RenderWorldOffscreenPrepassPipeline @ 0x123190` is the shadow pipeline. It
  renders the depth prepass into `textures[0x43ebe414]` cleared to (1,1,1,0),
  then attaches `textures[0x43ebe408]` (the shadow map) cleared to (0,0,0,0),
  masks color and depth, sets `GL_GEQUAL` (or `GL_LEQUAL` with the reversed-Z
  flag at `0x11e46194`), disables culling, and counts two-sided stencil with
  `INCR_WRAP` on back faces and `DECR_WRAP` on front faces (z-fail). After the
  volumes it draws one full-screen quad with color mask (1,1,0,0), stencil test
  `EQUAL 0`, and the color at `0x2796e0` = (1.0, 0.5, 1.0). The shadow factor is
  therefore binary: red = 1 where lit, 0 where in shadow. The shadow-volume
  fragment color (0,1,0,0.0625) is never written; color writes are masked.
- `WorldShading @ 0xa61f0` computes `lightambient` (`0x44178d40`) and
  `lightdiffuse` (`0x44178d50`) every frame from the light direction at
  `0x4417de78`. Optical depth is `sqrt(512^2 - 496^2 + dy^2*496^2) - dy*496`
  (constants `0x278aec`, `0x278ae8`, `0x278958`) scaled by `0.029296875`
  (`0x278174`); the four horizontal and one zenith probes also carry
  `sqrt(0.09765625)` (`0x2781e8`). Wavelength factors at `0x2b23d0` are
  (0.5, 0.625, 1.0). `lightdiffuse = 1.125 * exp(-w * depth(light.y))`;
  `lightambient = (0.5 * sum(4 horizon probes) + 0.75 * zenith) * 0.75 *
  (1 - (1 - light.y)^4)`. The world shader only reads `lightdiffuse`; the
  landscape shader reads both.
- `Render_SetupShadowMapPass @ 0x94a90` is misnamed: it renders the water
  reflection plane into `textures[0x43ebe428]` (clear color 0.375) and sets the
  clip planes; it is not part of the shadow path.
- The sky uniforms at `0x2f61b8` are atmoconstant (0.0502275936, 0.0001, 6.4,
  637.1) and wavelengths (0.625, 0.75, 1.0), matching the editor sky shader.

The editor now ports `WorldShading` (`game_light_colors`), samples per-face
averages of the analytic sky plus a lit ground color in place of the lod-8
landscape cubemap, lights terrain with the landscape equation, and adds the
array-world window shine term. Clouds and the real rendered cubemap are still
not reproduced.

## 2026-09-18 map limits (Ghidra, subrosa.exe)

Capacities are static array sizes; the loaders do not bounds-check, so
exceeding them corrupts neighbouring globals rather than failing cleanly.

| Thing | Limit | Evidence |
| --- | ---: | --- |
| Junctions (intersections) | 1024 | `CitySbc_Load @0xd5a50` array `0x2e48687c`, 12 B each, ends at the street count `0x2e48987c` |
| Roads (streets) | 669 slack, no runtime check | file array `0x2e489880`, 52 B each, until `g_cityBuildingDataCount @0x2e492084`; `MapConnector_AddStreetSegment @0x38e0` never checks |
| Buildings | 8192 | `g_cityBuildingRecords @0x35efb098`, 0x60 B each, until `g_cityPathCount @0x35fbb094` |
| City blocks (building data) | 4096 | `g_cityBuildingDataName @0x2e492088`, 0x4248 B each, until `g_cityDistrictCount @0x326da088` |
| Piece portals (districts) | 16384 | `g_cityDistrictId @0x326da08c`, 0xe04 B each, until the header records `0x35eea08c` |
| Header records | 1088 | `0x35eea090`, 64 B each, until `g_cityBuildingCount @0x35efb090` |
| World cells | x,z 0..4095, y 0..511 | `_createarea(0, ..., 64, 8, 64)` chunks of 64 cells; `World_SetCellBlockTemplateId @0x16f60` rejects negatives and `>> 6` beyond the chunk grid |
| Active chunks (64^3 cells) | 512 | `World_AllocateAreaChunkIndex @0x10020` slot loop `< 0x200` |
| Sectors (8^3 cells) | 4096 | sector tables stride 0x1b12, loops to 0x1000 |
| Building footprint | 63 x 63 cells | `Sbb_LoadFromFile @0x26830` cell index `x + (floor*64 + depth)*64`, loops run `<= size` |
| Building floors | 192 | cell array `0x58 .. 0x2d006ac` at 60 B per cell = 64*64*192 |
| Building materials / buildblock names | 320 / 1088 | name tables at `+0x2d006b0` and `+0x2d056b4`, 64 B each |
| Placement y | anchor layer level | `CitySbc_Load` pairs `size_z` with `anchor_y`; floors below the anchor are foundation |
