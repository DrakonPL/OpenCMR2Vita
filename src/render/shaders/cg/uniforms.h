// Row offsets (float4 rows) of the GXM uniform arrays: vu[] of fixed3d_v.cg
// and fu[] of the fixed_f variants. Read by the Cg programs and the GXM
// executor (render/gfx_gxm.cpp), so plain #defines only. Flags are 0 or 1.
#ifndef OPENCMR2_GXM_UNIFORMS_H
#define OPENCMR2_GXM_UNIFORMS_H

// ---- vu[]: fixed3d_v.cg ----
// Matrices are Direct3D's row-vector matrices, one row per float4.
#define VU_WORLD_VIEW 0
#define VU_PROJECTION 4
#define VU_TEXTURE_MATRIX0 8
#define VU_TEXTURE_MATRIX1 12
#define VU_MATERIAL_DIFFUSE 16
#define VU_MATERIAL_AMBIENT 17
#define VU_MATERIAL_SPECULAR 18
#define VU_MATERIAL_EMISSIVE 19
#define VU_GLOBAL_AMBIENT 20    // w: material power
#define VU_LIGHTS 21            // VU_LIGHT_ROWS rows for each of the 8 lights
#define VU_LIGHT_ROWS 7
#define VU_LIGHT_DIFFUSE 0
#define VU_LIGHT_SPECULAR 1
#define VU_LIGHT_AMBIENT 2
#define VU_LIGHT_POSITION 3     // eye space; w: type (1 point, 2 spot, 3 directional)
#define VU_LIGHT_DIRECTION 4    // eye space; w: range
#define VU_LIGHT_ATTENUATION 5  // a0, a1, a2, falloff
#define VU_LIGHT_SPOT 6         // cos(theta / 2), cos(phi / 2), enabled, -
#define VU_LIGHT_FLAGS 77       // lighting, colour vertex, specular, local viewer
#define VU_SOURCES 78           // diffuse, specular, ambient, emissive: 0 material, 1 colour 1, 2 colour 2
#define VU_FOG_FLAGS 79         // linear vertex fog, normalise normals, fog from specular alpha, -
#define VU_FOG_PARAMS 80        // start, end, -, -
#define VU_TEX_COORD 81         // source of stage 0, of stage 1 (0 uv0, 1 uv1, 2 reflection); transform 0, 1
#define VU_TARGET 82            // -, -, half-pixel offset x, y (NDC)
#define VU_ROWS 83

// ---- fu[]: fixed_f variants ----
// Per stage, each of the four arguments (colour 1, colour 2, alpha 1,
// alpha 2): its source (0 diffuse, 1 current, 2 texture, 3 texture factor,
// 4 specular), its complement flag and its alpha replicate flag.
#define FU_STAGE0_SOURCE 0
#define FU_STAGE0_COMPLEMENT 1
#define FU_STAGE0_REPLICATE 2
#define FU_STAGE1_SOURCE 3
#define FU_STAGE1_COMPLEMENT 4
#define FU_STAGE1_REPLICATE 5
#define FU_TEXTURE_FACTOR 6
#define FU_ALPHA_TEST 7         // compare function (GFX_CMP_*), reference 0..255, -, -
#define FU_FOG_COLOR 8
#define FU_FOG_PARAMS 9         // start, end, -, -
#define FU_BUMP_MATRIX 10       // m00, m01, m10, m11 of the bump stage
#define FU_BUMP_LUMINANCE 11    // scale, offset, -, -
#define FU_DISTANCE_FADE 12     // start, end, -, -
#define FU_TEX_SIZE0 13         // stage 0 texture width, height; its original's (vector icon) width, height
#define FU_TEX_SIZE1 14
#define FU_TARGET 15            // 1 / render target width, 1 / height, -, -
#define FU_ROWS 16

#endif
