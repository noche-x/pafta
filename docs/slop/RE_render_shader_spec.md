# How Sub Rosa renders: shaders and passes

These notes record traces of the game's shaders and the fixes made to Pafta afterwards. The original shader code captured from the game is in the sibling repository at `../../client/src/shaders/stock`. Check it, along with how its render pass is set up, before changing any approximation in Pafta.

## The game's shader programs

The Windows binary keeps the shader source text at these addresses. The renderer builds the programs in `renderInitShadersAndWriteReport` and stores them in `shaderInfos`.

| Program | Vertex | Fragment | Geometry | Use |
| --- | ---: | ---: | ---: | --- |
| 0 | `0x2f61e0` | `0x2f6460` | none | world objects and blocks |
| 1 | `0x2f7bb0` | `0x2f7e60` | none | array-world objects |
| 2 | `0x2f9820` | `0x2f9a10` | none | reflective objects |
| 3 | `0x2f9fd0` | `0x2fa480` | none | models |
| 5 | `0x2fc070` | `0x2fc310` | none | depth prepass |
| 6 | `0x2fc620` | `0x2fc780` | none | sky |
| 8 | `0x2fe670` | `0x2fe9a0` | none | landscape |
| 9 | `0x2ff460` | `0x2ff4e0` | `0x2ff550` | shadow volumes |
| 10 | `0x3006e0` | `0x300830` | none | cubemap blur |
| 11 | `0x300dd0` | `0x300ec0` | none | HDR composite |
| 12 | `0x3012a0` | `0x301390` | none | water |

The exact captured shaders are in the sibling client repository:

- `../../client/src/shaders/stock/world_vertex.hpp`
- `../../client/src/shaders/stock/world_fragment.hpp`
- `../../client/src/shaders/stock/landscape_vertex.hpp`
- `../../client/src/shaders/stock/landscape_fragment.hpp`
- `../../client/src/shaders/stock/sky_vertex.hpp`
- `../../client/src/shaders/stock/sky_fragment.hpp`
- `../../client/src/shaders/stock/shadow_volume_vertex.hpp`
- `../../client/src/shaders/stock/shadow_volume_geometry.hpp`
- `../../client/src/shaders/stock/shadow_volume_fragment.hpp`

The source addresses and uniform setup were recovered in `../../git-recovered-blobs/blob_9.txt`. Linux function addresses are in `../../client/src/offsets.hpp`.

## World materials and shadows

The world vertex shader takes a position, a three-part texture coordinate, and a normal at locations 0, 1, and 2. It passes the world position, normal, light vector, and view vector on to the fragment shader.

The fragment shader uses these texture units:

| Unit | Uniform | Meaning |
| ---: | --- | --- |
| 0 | `tex_color` | base color, sRGB |
| 1 | `tex_shadowmap` | shadow factor in screen space |
| 2 | `tex_material` | packed material map, sRGB in the original shader |
| 3 | `tex_cubemap` | environment reflection |
| 4 | `tex_atlas_color` | array overlay color |
| 5 | `tex_atlas_material` | array overlay material |

It turns the base color and material map into linear color, uses the material's red channel as metallic and green as roughness, and works out GGX and Fresnel lighting. It multiplies the direct light by the shadow factor, adds diffuse and specular light from the cubemap, and turns the result back into sRGB. The shadow factor is read with `texture(tex_shadowmap, gl_FragCoord.xy * ooresolution).r`. It is not baked into vertex colors and it is not a stencil value.

The game's shadow-volume program uses adjacency geometry. The geometry shader gets `triangles_adjacency`, finds faces turned away from the light with `dot(faceNormal, lightdirection)`, and pushes them out by `-lightdirection * shadowlength`. It only adds side quads on edges where the neighbouring face faces the light, and it adds front and back caps. Its fragment shader writes `vec4(0, 1, 0, 0.0625)`. This buffer becomes the red shadow factor that the world and model shaders read.

## Landscape

The landscape vertex shader works out these coordinates from the world position:

```text
color.s = (x / 8 + 512) / 2048
color.t = 1 - (z / 8 + 512) / 2048
detail.s = x / 32
detail.t = 1 - z / 32
mask.s = (x / 8 + 512) / 2048
mask.t = (z / 8 + 512) / 2048
```

It also picks a tyre-mark layer with `floor(position.xz / 128) & 15`, which gives 256 layers that wrap. The fragment shader uses `tex_normal` on unit 0, `tex_detail` on unit 1, `tex_color` on unit 2, `tex_mask` (unsigned integer) on unit 3, and `tex_shadow` on unit 4. If `land_usemask == 1`, it reads one bit from the 16-bit mask texel and throws the pixel away when that bit is clear.

Terrain lighting does not use the world PBR path. It uses `rho = 3` and `k = 4/3`, then works out a wrapped diffuse scale from the dot product of the normal and the light. It multiplies the linear detail and color textures by:

```text
lightambient * (0.375 + dot(normal, light) * 0.125)
+ lightdiffuse * scale * shadow * 0.625
```

It then adds a view-dependent shine based on the detail texture, applies the tyre marks, and converts to sRGB. So terrain needs its own shader and all five of the game's texture inputs. Using a generic colored-mesh shader changes both how textures line up and how light falls.

## Sky

The sky program uses `tex_depth` on unit 0, `atmoconstant`, `atmowavelength`, `ooresolution`, `worldposition`, `lightdirection`, and `numofsamples = 4`. The default atmosphere values are at Windows `0x2f61b8` and `0x2f61c8`, and at Linux `0x3889f0` and `0x3889e0`. The shader does atmospheric scattering that takes depth into account, and only blends in clouds for pixels whose packed depth is greater than 65536. The cloud blend comes from `SRC_CLOUD_SHADER_SOURCE` in the client build. A sky shader without it is not the full game sky.

## Pass order and entry points

The Windows functions that matter:

| Address | Function |
| ---: | --- |
| `0x11fd00` | `renderWorld` |
| `0x122690` | `renderSky` |
| `0x946d0` | `renderShadows` |
| `0x93e60` | landscape cubemap capture and blur |

On Linux they are `0x127844`, `0x54022`, `0xc49cb`, and `0x8bab5`. The depth prepass stores the distance from the camera in RGB. The sky reads that packed depth to tell geometry apart from far-away sky. Shadow volumes are drawn into the shadow buffer before the world and landscape color passes. Landscape chunks then read the same screen-space shadow result as world objects.

## Differences found in the first pass

Later sections correct some of these.

1. The `src/render_gpu.cpp` that was checked wrote its own GLSL instead of reusing the captured landscape, world, sky, and shadow-volume shaders. Its sky had no clouds, and its terrain had no normal texture and no unsigned land-use mask.
2. The editor's `LitShader` used its own `atmosphereWorldPosition` and worked out the atmosphere directly inside the world shader. In the game, world objects get their environment light from the cubemap, and the sky program alone does the depth-aware atmosphere.
3. The editor uploaded road and terrain runs as generic mesh materials. To match the game, the road and terrain renderer has to supply the landscape texture UVs, `tex_detail`, `tex_color`, `tex_shadow`, and the mask, and then use the game's landscape lighting formula.

## Corrections from 2026-09-17

Checked again in `/subrosa.exe.THIS_ONE` with Ghidra:

- `RenderSky @ 0x122690` turns off depth and culling and uses `GL_ONE, GL_SRC_ALPHA`.
- `_glstateshadowtwoside @ 0xa6fb0` counts back and front faces with opposite wrapping stencil operations and a reversed depth test. The editor uses closed volumes with depth-fail counting under its normal depth test.
- `RenderQueuedShadowVolumeAdjacencyPass @ 0x11c950` scales how far buildings stretch by their height and by `8 / max(light.y, 0.125)`. The constants at `0x278728` and `0x278224` are `8.0` and `0.125`.
- The queued district pass at `0x946d0` uploads `32.0` from `0x2787d4` as its stretch length.
- The captured `stock/world_fragment.hpp` multiplies direct diffuse and specular by the red shadow factor before adding environment light.

The editor now writes packed depth on desktop as well as WebGL, turns its attachment on before clearing it, and protects against an atmosphere density of zero. It caches triangle adjacency for buildings and district meshes, closes outline sides with consistent winding, and stretches building shadows by height. Direct light is kept separately and removed where pixels are in shadow, before tone mapping; ambient light, reflections, and sky stay. Gizmos are drawn after the final resolve.

This lines the code up with the game's source. It does not claim the images match. The cubemap, cloud, and landscape differences above are still there. No build or runtime comparison was done for this change, as requested.

## Rule: the game's source decides

Do not tune constants in the editor's shaders until you have checked the captured original shader and the render pass it belongs to. If a texture is not available, keep the game's sampler and give it a stand-in texture with the same color space, channel order, wrap, and filtering. Never quietly replace a shadow map or terrain material map with something faked from vertex colors.

## Lighting and shadow findings from 2026-09-17 (Ghidra, subrosa.exe)

- `RenderWorldOffscreenPrepassPipeline @ 0x123190` is the shadow pipeline. Step by step:
  - It draws the depth prepass into `textures[0x43ebe414]`, cleared to (1,1,1,0).
  - It attaches `textures[0x43ebe408]` (the shadow map), cleared to (0,0,0,0).
  - It masks color and depth, sets `GL_GEQUAL` (or `GL_LEQUAL` when the reversed-Z flag at `0x11e46194` is set), and turns off culling.
  - It counts two-sided stencil with `INCR_WRAP` on back faces and `DECR_WRAP` on front faces (z-fail).
  - After the volumes, it draws one full-screen quad with color mask (1,1,0,0), stencil test `EQUAL 0`, and the color at `0x2796e0` = (1.0, 0.5, 1.0).

  So the shadow factor is all or nothing: red is 1 where lit and 0 in shadow. The shadow-volume fragment color (0,1,0,0.0625) is never written, because color writes are masked off.
- `WorldShading @ 0xa61f0` works out `lightambient` (`0x44178d40`) and `lightdiffuse` (`0x44178d50`) every frame from the light direction at `0x4417de78`. The optical depth is `sqrt(512^2 - 496^2 + dy^2*496^2) - dy*496` (constants `0x278aec`, `0x278ae8`, `0x278958`), scaled by `0.029296875` (`0x278174`). The four horizon samples and the one straight-up sample also carry `sqrt(0.09765625)` (`0x2781e8`). The wavelength factors at `0x2b23d0` are (0.5, 0.625, 1.0). Then `lightdiffuse = 1.125 * exp(-w * depth(light.y))` and `lightambient = (0.5 * sum(4 horizon probes) + 0.75 * zenith) * 0.75 * (1 - (1 - light.y)^4)`. The world shader only reads `lightdiffuse`; the landscape shader reads both.
- `Render_SetupShadowMapPass @ 0x94a90` has the wrong name. It draws the water reflection into `textures[0x43ebe428]` (clear color 0.375) and sets the clip planes. It has nothing to do with shadows.
- The sky uniforms at `0x2f61b8` are atmoconstant (0.0502275936, 0.0001, 6.4, 637.1) and wavelengths (0.625, 0.75, 1.0), the same as the editor's sky shader.

The editor now ports `WorldShading` (`game_light_colors`). Instead of the game's lod-8 landscape cubemap, it averages the computed sky and a lit ground color for each face. It lights terrain with the landscape formula and adds the array-world window shine. Clouds and the real rendered cubemap are still missing.

## Map limits from 2026-09-18 (Ghidra, subrosa.exe)

These limits are the sizes of fixed arrays. The loaders do not check them, so going over a limit overwrites the data next to it instead of failing cleanly.

| Thing | Limit | Evidence |
| --- | ---: | --- |
| Junctions (intersections) | 1024 | `CitySbc_Load @0xd5a50` array `0x2e48687c`, 12 B each, ends at the street count `0x2e48987c` |
| Roads (streets) | 669 spare slots, never checked | file array `0x2e489880`, 52 B each, until `g_cityBuildingDataCount @0x2e492084`; `MapConnector_AddStreetSegment @0x38e0` never checks |
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

## Lighting, shadows, and the map image, 2026-09-25 (Ghidra, subrosa.exe)

Done in `render_gpu.cpp` (`update_environment_cubemap`, `landscape_detail_texture`, `draw_city_shadow_volumes`, `city_map_image`) and `ui_windows.cpp` (`draw_map_image_window`).

### Environment cubemap

`RenderLandscapeCubemapAndBlurCapture @93e60` draws six 512 faces around the camera, each cleared to 0.125. `_renderframeatmospherecubemap @122ca0` fills them with the sky program over a full-screen quad (no blending). Then it draws the landscape at half `lightambient`/`lightdiffuse` using `g_texLandscapeCubemapColor`. That texture is the landscape color map, grey (0x54) over texels 0x100..0x1bf x 0x240..0x2ff (world x, z 0..3072). `_blurcubemap @1b6a0` fills mip levels 1..9 (256 down to 1) with the original blur shader, each from the level above. `world.frag` reads it at lod 8 for diffuse and `roughness^2 * 8` for specular; the window shine in `array_world.frag` reads lod 0. The editor captures with GL cube directions and samples with the true directions (the game flips y and z on both sides). It captures again when the sun or terrain changes or the camera moves 16 units.

### Landscape detail

`_landscape_generatetexture @d4360` makes a 1024x1024 texture: n = 57y + x, n ^= n << 13, v = ((n*n*15731 + 789221)^2 - 0x2df722f3) & 0x7fffffff, grey = int(16 - v * 2^-26 + 192). The color map is a flat (0x40, 0x54, 0x26). `landscape.vert` reads detail at (x/32, 1 - z/32). The captured `landscape.frag` takes its normal from the vertex, not from `tex_normal`.

### HDR composite

Reinhard `c / (c + 1)`, then sRGB. There is no anti-aliasing pass.

### Shadows

In `RenderQueuedShadowVolumeAdjacencyPass @11c950`, every world sector stretches all of its triangles (window panes too) by `(sector y + 1) * 32 / max(light.y, 0.125)`. The editor stretches each building by its top sector, and the road and curb tile mesh by its highest sector. Models (street objects) stretch 64 (`DAT_00278814`, `@124fe0`).

### Shadow volume shape

From `stock/shadow_volume_geometry.hpp`: for each triangle, `faceslight` is `dot(normal, lightdirection) >= 0`, using the outward normal of the game's clockwise faces. A triangle that does not face the light gets a front cap, a back cap pushed by `-lightdirection * shadowlength`, and a side on each edge whose neighbour faces the light or is missing. A missing neighbour repeats a vertex of the triangle, so its normal is zero and it counts as facing the light.

The editor has no geometry shader, so `build_shadow_volume` bakes one mesh per model that works for any light direction. It has both caps for every render triangle and a quad for every edge, where each end of the quad carries its own face's normal. The vertex shader drops the caps of lit faces and stretches the edge ends of lit faces, so only the outline quads end up with any area. The sun and stretch length are uniforms, and the rotation is the instance matrix. The caps reuse the exact render triangles (with `invariant gl_Position`) so their depth matches the surface. Edges are split at T-junctions (window cut-outs) and paired a->b with b->a, so overlapping open sides never let light through.

### Map image

`GenerateMapTextureFromVisibleSectors @124300` makes a 512x512 image:

- It draws sectors only (no landscape) with the image shader (base texture, no lighting) on black.
- The camera is orthographic with a half-size of 1024 at (1920, 1920), so it covers world x and z 896..2944 at one pixel per cell.
- It is skipped when `isMapAutoGenerated` is set (the map is named `round`), which keeps `map.png`.

`DrawMapMenu @88130` draws it as 256 map units and prints street names (size 6, centred) at `((A + B) * 2 - 896) / 8`:

- along X, on the segment whose start has no street coming from -X;
- along Z, on the segment starting at gz 320..323;
- +16 map units on odd avenue columns.

Its view is orthographic with a half-size of 128 around map (120, 120) on city maps, or (128, 128) when auto-generated. The viewport is 512x512 of a 1024x768 virtual screen, and the image is drawn at alpha 0.75. `mapLegendPos` is (896, 896), set by `GenerateMapTextureFromVisibleSectors`. Labels use `renderText` flags 5: flag 1 moves x back by half the total advance, and flag 4 moves y back by half the size. The font is `textFont` 2, "rockwell", which `RenderWorld` sets before the HUD. Glyphs are `_cs_drawtextstring` quads, size times the advance wide and size tall, from the 16x16 atlas. Advances are `(base - 0.5) / cell height`. The client version is `drawMapMenuLocal` in `client/src/features/hud.cpp`. Company names (building type 1) and `$` (type 6) come from building data at runtime, and the editor does not draw them.

### Other findings

- `_resetgame` sets `isMapAutoGenerated` when the map is named `round`. It also forces street lengths to 6 or 10; the editor's road placement does not do this yet.
- Culling: world geometry is wound clockwise, and the game draws it with `glFrontFace(GL_CW)` and back faces culled (`GenerateMapTextureFromVisibleSectors` turns on `GL_CULL_FACE` after `glFrontFace(0x900)`). The editor culls `GL_FRONT` under raylib's counter-clockwise front face, which removes the same faces, for every lit surface. Masks and shadow volumes stay two-sided.
