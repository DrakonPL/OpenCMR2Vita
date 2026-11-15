/*
 * OpenCMR2 renderer interface, implemented in src/render on SDL_GPU.
 *
 * It exposes the rendering model CMR2 was written for (Direct3D 7's
 * fixed-function pipeline) as immediate-mode calls on our own types: state
 * setters, then draws that use the current state. Draws are recorded with a
 * copy of their vertices and indices, so the game may overwrite its buffers
 * right after drawing, as it could with Direct3D.
 *
 * Enumerated values equal the original Direct3D constants, because the game
 * keeps them in its data and passes them around as plain numbers.
 */
#ifndef OPENCMR2_PORT_GFX_H
#define OPENCMR2_PORT_GFX_H

#include "port/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- types ---------------------------------------------------------------- */

typedef struct GfxTexture GfxTexture;
typedef struct GfxVertexBuffer GfxVertexBuffer;

/* Row-vector matrix, Direct3D layout: v' = v * M, translation in _41.._43. */
typedef struct GfxMatrix {
    float _11, _12, _13, _14;
    float _21, _22, _23, _24;
    float _31, _32, _33, _34;
    float _41, _42, _43, _44;
} GfxMatrix;

typedef struct GfxVector {
    float x, y, z;
} GfxVector;

typedef struct GfxColorValue {
    float r, g, b, a;
} GfxColorValue;

/* Colours are 0xAARRGGBB. */
#define GFX_RGBA(r, g, b, a) \
    ((DWORD)((((a) & 0xff) << 24) | (((r) & 0xff) << 16) | (((g) & 0xff) << 8) | ((b) & 0xff)))

/* 3D vertex: position, normal, diffuse, specular, two texture coordinate
   sets (Direct3D FVF 0x2d2, 48 bytes). Lit and transformed by the device. */
typedef struct GfxVertex {
    float x, y, z;
    float nx, ny, nz;
    DWORD diffuse;
    DWORD specular;
    float u0, v0;
    float u1, v1;
} GfxVertex;

/* Pre-transformed 2D vertex: screen position, depth, 1/w, colours, one
   texture coordinate set (D3DTLVERTEX, 32 bytes). */
typedef struct GfxTLVertex {
    float sx, sy, sz, rhw;
    DWORD color;
    DWORD specular;
    float tu, tv;
} GfxTLVertex;

enum GfxLightType {
    GFX_LIGHT_POINT = 1,
    GFX_LIGHT_SPOT = 2,
    GFX_LIGHT_DIRECTIONAL = 3,
    GFX_LIGHT_PARALLELPOINT = 4
};

typedef struct GfxLight {
    int type;
    GfxColorValue diffuse;
    GfxColorValue specular;
    GfxColorValue ambient;
    GfxVector position;
    GfxVector direction;
    float range;
    float falloff;
    float attenuation0, attenuation1, attenuation2;
    float theta, phi;
} GfxLight;

typedef struct GfxMaterial {
    GfxColorValue diffuse;
    GfxColorValue ambient;
    GfxColorValue specular;
    GfxColorValue emissive;
    float power;
} GfxMaterial;

typedef struct GfxViewport {
    DWORD x, y, width, height;
    float minZ, maxZ;
} GfxViewport;

enum GfxPrimitive {
    GFX_POINTLIST = 1,
    GFX_LINELIST = 2,
    GFX_LINESTRIP = 3,
    GFX_TRIANGLELIST = 4,
    GFX_TRIANGLESTRIP = 5,
    GFX_TRIANGLEFAN = 6
};

enum GfxTransformType {
    GFX_TRANSFORM_WORLD = 1,
    GFX_TRANSFORM_VIEW = 2,
    GFX_TRANSFORM_PROJECTION = 3,
    GFX_TRANSFORM_TEXTURE0 = 16,
    GFX_TRANSFORM_TEXTURE1 = 17
};

enum GfxCompare {
    GFX_CMP_NEVER = 1,
    GFX_CMP_LESS = 2,
    GFX_CMP_EQUAL = 3,
    GFX_CMP_LESSEQUAL = 4,
    GFX_CMP_GREATER = 5,
    GFX_CMP_NOTEQUAL = 6,
    GFX_CMP_GREATEREQUAL = 7,
    GFX_CMP_ALWAYS = 8
};

enum GfxBlend {
    GFX_BLEND_ZERO = 1,
    GFX_BLEND_ONE = 2,
    GFX_BLEND_SRCCOLOR = 3,
    GFX_BLEND_INVSRCCOLOR = 4,
    GFX_BLEND_SRCALPHA = 5,
    GFX_BLEND_INVSRCALPHA = 6,
    GFX_BLEND_DESTALPHA = 7,
    GFX_BLEND_INVDESTALPHA = 8,
    GFX_BLEND_DESTCOLOR = 9,
    GFX_BLEND_INVDESTCOLOR = 10,
    GFX_BLEND_SRCALPHASAT = 11
};

enum GfxCull {
    GFX_CULL_NONE = 1,
    GFX_CULL_CW = 2,
    GFX_CULL_CCW = 3
};

enum GfxFogMode {
    GFX_FOG_NONE = 0,
    GFX_FOG_EXP = 1,
    GFX_FOG_EXP2 = 2,
    GFX_FOG_LINEAR = 3
};

/* Where a material colour comes from when vertex colours are enabled. */
enum GfxMaterialSource {
    GFX_MCS_MATERIAL = 0,
    GFX_MCS_COLOR1 = 1,
    GFX_MCS_COLOR2 = 2
};

enum GfxTextureOp {
    GFX_TOP_DISABLE = 1,
    GFX_TOP_SELECTARG1 = 2,
    GFX_TOP_SELECTARG2 = 3,
    GFX_TOP_MODULATE = 4,
    GFX_TOP_MODULATE2X = 5,
    GFX_TOP_MODULATE4X = 6,
    GFX_TOP_ADD = 7,
    GFX_TOP_ADDSIGNED = 8,
    GFX_TOP_ADDSIGNED2X = 9,
    GFX_TOP_SUBTRACT = 10,
    GFX_TOP_BUMPENVMAP = 22,
    GFX_TOP_BUMPENVMAPLUMINANCE = 23
};

/* Texture stage arguments, optionally combined with the two modifiers. */
enum GfxTextureArg {
    GFX_TA_DIFFUSE = 0,
    GFX_TA_CURRENT = 1,
    GFX_TA_TEXTURE = 2,
    GFX_TA_TFACTOR = 3,
    GFX_TA_SPECULAR = 4,
    GFX_TA_COMPLEMENT = 0x10,
    GFX_TA_ALPHAREPLICATE = 0x20
};

enum GfxFilter {               /* minification and magnification */
    GFX_FILTER_POINT = 1,
    GFX_FILTER_LINEAR = 2
};

enum GfxMipFilter {
    GFX_MIPFILTER_NONE = 1,
    GFX_MIPFILTER_POINT = 2,
    GFX_MIPFILTER_LINEAR = 3
};

enum GfxAddress {
    GFX_ADDRESS_WRAP = 1,
    GFX_ADDRESS_MIRROR = 2,
    GFX_ADDRESS_CLAMP = 3
};

/* Texture coordinate index of a stage: the vertex set in the low bits,
   optionally generated instead. */
#define GFX_TCI_CAMERASPACEREFLECTIONVECTOR 0x30000

/* Texture transform: number of coordinates it outputs. */
enum GfxTextureTransform {
    GFX_TTFF_DISABLE = 0,
    GFX_TTFF_COUNT2 = 2,
    GFX_TTFF_COUNT3 = 3
};

/* ---- device and frame ------------------------------------------------------ */

/* The SDL window flags the renderer needs, set up before the window is made
   (SDL_WindowFlags; OpenGL's context is chosen with its window). */
unsigned long long Gfx_PrepareWindow(void);
/* The renderer's name for the graphics menu: "OpenCMR2 SDL_GPU" or
   "OpenCMR2 OpenGL". */
const char *Gfx_GetRendererName(void);
/* Creates the GPU device on first use and sets the back buffer size and
   window mode (it may be called again to change them). */
BOOL Gfx_SetVideoMode(int width, int height, BOOL fullscreen);
/* Switches the window between fullscreen and windowed, keeping the mode. */
void Gfx_SetFullscreen(BOOL fullscreen);
void Gfx_Shutdown(void);

/* Resolutions to offer, smallest first. */
int Gfx_GetDisplayModeCount(void);
void Gfx_GetDisplayMode(int index, int *width, int *height);
/* The resolution of a new configuration, one of the modes offered: the
   display's own. */
void Gfx_GetDefaultDisplayMode(int *width, int *height);

/* A frame is BeginScene...EndScene blocks (any number) and one Present. */
void Gfx_BeginScene(void);
void Gfx_EndScene(void);
/* vsync: the refreshes of the display for each frame: 0 at once, 1 the
   next refresh, 2 every second refresh (30 frames a second at 60 Hz). */
void Gfx_Present(BOOL vsync);
/* Movie presentation: stretch the back buffer to the entire drawable area. */
void Gfx_PresentStretched(BOOL vsync);

enum GfxClearFlags {
    GFX_CLEAR_TARGET = 1,
    GFX_CLEAR_ZBUFFER = 2
};
void Gfx_Clear(DWORD flags, DWORD color, float z);
void Gfx_SetViewport(const GfxViewport *viewport);
void Gfx_GetViewport(GfxViewport *viewport);
/* The size of the screen the game draws the back buffer in: its viewports,
   clears and pre-transformed vertices are scaled from it to the back
   buffer's pixels, which are still rasterised at their resolution. 0, 0 is
   the back buffer's own size. Render-target textures are not scaled. */
void Gfx_SetVirtualScreen(int width, int height);

/* Preserve drawing state around temporary passes such as movie playback.
   Referenced textures must remain alive until the matching PopState. */
void Gfx_PushState(void);
void Gfx_PopState(void);

/* Draws into a render-target texture (or a cube face from Gfx_GetCubeFace)
   or, with NULL, the back buffer. */
void Gfx_SetRenderTarget(GfxTexture *target);

/* Distance fade: keeps a copy of the back buffer as it is now (the sky and
   the backdrop, drawn first), and 3D draws into the back buffer then blend
   into that copy between distances start and end from the eye, so the
   scenery dissolves into what lies behind it before the far plane cuts it. end <= start turns
   the fade off. */
void Gfx_CaptureBackdrop(void);
void Gfx_SetDistanceFade(float start, float end);

/* The draws of the sky: begin TRUE before them, FALSE after them. An
   executor can draw them in another way (the GXM executor draws them into
   the backdrop, and leaves out the pixels that the near ground covers).
   Into a render target: nothing. */
void Gfx_MarkSky(BOOL begin);
/* Whether the renderer can draw the sky at a smaller size and stretch it
   (video.sky_half). */
BOOL Gfx_SupportsHalfSky(void);

/* ---- fixed-function state -------------------------------------------------- */

void Gfx_SetTransform(int type, const GfxMatrix *matrix);
void Gfx_GetTransform(int type, GfxMatrix *matrix);

void Gfx_SetLight(int index, const GfxLight *light);
void Gfx_GetLight(int index, GfxLight *light);
void Gfx_EnableLight(int index, BOOL enable);
void Gfx_SetMaterial(const GfxMaterial *material);

/* One setter per render state the game uses; the values are Direct3D's. */
void Gfx_SetLighting(BOOL enable);
void Gfx_SetAmbient(DWORD color);
void Gfx_SetSpecular(BOOL enable);
void Gfx_SetLocalViewer(BOOL enable);
void Gfx_SetNormalizeNormals(BOOL enable);
/* The second texture coordinates of each vertex from the vertex itself:
   with m = (position + normal) / 2 in model space,
   u1 = m.x * rows[0] + m.y * rows[1] + m.z * rows[2] + rows[3], and
   v1 the same with rows[4..7]. NULL: the coordinates of the vertex. The
   game's car reflection computed them on the CPU in each frame. */
void Gfx_SetTexCoord1Generation(const float rows[8]);
void Gfx_SetColorVertex(BOOL enable);
void Gfx_SetDiffuseMaterialSource(int source);
void Gfx_SetSpecularMaterialSource(int source);
void Gfx_SetAmbientMaterialSource(int source);
void Gfx_SetEmissiveMaterialSource(int source);

void Gfx_SetFog(BOOL enable);
void Gfx_SetFogColor(DWORD color);
void Gfx_SetFogVertexMode(int mode);
void Gfx_SetFogTableMode(int mode);
void Gfx_SetFogStart(float start);
void Gfx_SetFogEnd(float end);

void Gfx_SetDepthTest(BOOL enable);
void Gfx_SetDepthWrite(BOOL enable);
void Gfx_SetDepthFunc(int compare);
void Gfx_SetCullMode(int cull);
void Gfx_SetAlphaBlend(BOOL enable);
void Gfx_SetSrcBlend(int blend);
void Gfx_SetDestBlend(int blend);
void Gfx_SetAlphaTest(BOOL enable);
void Gfx_SetAlphaFunc(int compare);
void Gfx_SetAlphaRef(DWORD reference);
void Gfx_SetTextureFactor(DWORD color);

/* Texture stages 0 and 1; stage 2 is accepted and ignored (no draw uses
   more than two). */
void Gfx_SetTexture(int stage, GfxTexture *texture);
void Gfx_SetStageColorOp(int stage, int op);
void Gfx_SetStageColorArg1(int stage, int arg);
void Gfx_SetStageColorArg2(int stage, int arg);
void Gfx_SetStageAlphaOp(int stage, int op);
void Gfx_SetStageAlphaArg1(int stage, int arg);
void Gfx_SetStageAlphaArg2(int stage, int arg);
void Gfx_SetStageAddress(int stage, int address);
void Gfx_SetStageMinFilter(int stage, int filter);
void Gfx_SetStageMagFilter(int stage, int filter);
void Gfx_SetStageMipFilter(int stage, int filter);
void Gfx_SetStageMaxMipLevel(int stage, int level);
void Gfx_SetStageTexCoordIndex(int stage, DWORD index);
void Gfx_SetStageTextureTransform(int stage, int flags);
/* index: 0 = m00, 1 = m01, 2 = m10, 3 = m11 */
void Gfx_SetStageBumpEnvMat(int stage, int index, float value);
void Gfx_SetStageBumpEnvLScale(int stage, float value);
void Gfx_SetStageBumpEnvLOffset(int stage, float value);

/* ---- geometry ---------------------------------------------------------------- */

GfxVertexBuffer *Gfx_CreateVertexBuffer(DWORD vertexCount);
void Gfx_DestroyVertexBuffer(GfxVertexBuffer *buffer);
/* The whole buffer, for writing; contents are kept between locks. */
GfxVertex *Gfx_LockVertexBuffer(GfxVertexBuffer *buffer);
/* As Gfx_LockVertexBuffer, for a caller that writes only vertexCount
   vertices from firstVertex (one mesh of a buffer of many) before the
   unlock: the draws of the other vertices are not those of a buffer that
   changed. Returns the whole buffer, as Gfx_LockVertexBuffer does. */
GfxVertex *Gfx_LockVertexBufferRange(GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount);
/* Copies vertexCount vertices into the buffer from firstVertex, as a
   Gfx_LockVertexBufferRange, a copy and an unlock do. The renderer sees what
   changes: vertices that the buffer has already change nothing, and
   positions that stay keep what it knows of them. */
void Gfx_WriteVertexBufferRange(GfxVertexBuffer *buffer, DWORD firstVertex, const GfxVertex *vertices,
                                DWORD vertexCount);
void Gfx_UnlockVertexBuffer(GfxVertexBuffer *buffer);

void Gfx_DrawPrimitiveVB(int primitive, GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount);
/* A draw whose vertices the caller gives in parts: Gfx_BeginPrimitive, then
   the vertices with Gfx_AddPrimitiveVertices, then Gfx_EndPrimitive, which
   makes the draw of them with the state it finds (no draw for no vertex).
   As a draw of the same vertices from a vertex buffer that the caller
   filled, without that buffer: the renderer copies each vertex once. No
   other draw between the begin and the end. */
void Gfx_BeginPrimitive(int primitive);
void Gfx_AddPrimitiveVertices(const GfxVertex *vertices, DWORD vertexCount);
/* The box of the positions of all the vertices of the primitive, from the
   caller that measured them: Gfx_EndPrimitive then does not. */
void Gfx_SetPrimitiveBox(const float low[3], const float high[3]);
void Gfx_EndPrimitive(void);
/* Whether a range of a vertex buffer can be in the view, with the current
   world, view and projection matrices: FALSE where each of its vertices is
   outside one plane of the view. TRUE where the renderer does not test
   draws against the view (the setting video.cull_draws). */
BOOL Gfx_IsRangeVisible(GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount);
/* The six planes of the view in world space, from the view and projection
   matrices that are set: four numbers (a, b, c, d) each. A point is outside
   the view when a * x + b * y + c * z + d is below zero for one plane.
   FALSE where the renderer does not test draws against the view: the
   caller then leaves nothing out. */
BOOL Gfx_GetViewPlanes(float planes[24]);
/* The box of the positions of a range of a vertex buffer. FALSE for a range
   that is not in the buffer. */
BOOL Gfx_GetRangeBox(GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount, float low[3], float high[3]);
/* A number of the buffer that changes when a lock or a write can have
   changed a position: a box that the caller keeps is good while it stays. */
DWORD Gfx_GetPositionSerial(GfxVertexBuffer *buffer);
/* The distance fade, for a caller that groups its draws by its start.
   worldView: the world matrix times the view matrix that are set (16
   numbers, a row-vector matrix). start: the distance from the eye where the
   fade starts. FALSE: no fade is set, or the renderer draws all distances
   with one program (also the setting video.fade_split = 0). The caller then
   does not group. */
BOOL Gfx_GetFadeView(float worldView[16], float *start);
/* Whether the game draws the ground parts of the visible sectors together,
   one draw for each vertex buffer and texture (the setting
   video.merge_ground; on by default where each draw costs much CPU time,
   on the PS Vita). */
BOOL Gfx_MergesGroundDraws(void);
/* Indices are relative to firstVertex. */
void Gfx_DrawIndexedPrimitiveVB(int primitive, GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount,
                                const WORD *indices, DWORD indexCount);
/* The same, from a caller that knows how far its vertices are. reach: no
   vertex that the indices use is farther from the eye, after the world and
   view matrices. The renderer then draws without the distance fade where
   the reach is below its start (the fade changes no pixel there). Below
   zero: not known, as Gfx_DrawIndexedPrimitiveVB. */
void Gfx_DrawIndexedPrimitiveVBReach(int primitive, GfxVertexBuffer *buffer, DWORD firstVertex, DWORD vertexCount,
                                     const WORD *indices, DWORD indexCount, float reach);
void Gfx_DrawPrimitive(int primitive, const GfxVertex *vertices, DWORD vertexCount);
void Gfx_DrawIndexedPrimitive(int primitive, const GfxVertex *vertices, DWORD vertexCount, const WORD *indices,
                              DWORD indexCount);
/* One draw of a batch (Gfx_DrawBatch): an indexed triangle list from a vertex
   buffer, with its world matrix and the texture of stage 0. The indices are
   relative to firstVertex. */
typedef struct GfxBatchDraw {
    const GfxMatrix *world;
    GfxTexture *texture;
    GfxVertexBuffer *buffer;
    DWORD firstVertex, vertexCount;
    const WORD *indices;
    DWORD indexCount;
} GfxBatchDraw;
/* The draws in their order, each as Gfx_SetTransform(GFX_TRANSFORM_WORLD,
   world), Gfx_SetTexture(0, texture) and Gfx_DrawIndexedPrimitiveVB(
   GFX_TRIANGLELIST, ...) draw it: the same pixels, and the same state after
   them. The vertices and the matrices are read during the call. For an
   executor that takes joined draws, the small draws (the trees of a stage)
   go into the frame as one batch: the renderer moves them into world space,
   tests them against the view and joins them when the frame runs. */
void Gfx_DrawBatch(const GfxBatchDraw *draws, DWORD count);
void Gfx_DrawPrimitiveTL(int primitive, const GfxTLVertex *vertices, DWORD vertexCount);
void Gfx_DrawIndexedPrimitiveTL(int primitive, const GfxTLVertex *vertices, DWORD vertexCount, const WORD *indices,
                                DWORD indexCount);

/* ---- textures -------------------------------------------------------------- */

enum GfxFormat {
    GFX_FORMAT_BGRX8,         /* 32-bit, no alpha */
    GFX_FORMAT_BGRA8,         /* 32-bit with alpha */
    GFX_FORMAT_BC1,           /* DXT1 */
    GFX_FORMAT_BC3,           /* DXT5 */
    GFX_FORMAT_BUMP_DUDVL     /* 24-bit: signed dU, signed dV, luminance (L8V8U8) */
};

enum GfxPixelFormatFlags {
    GFX_PF_ALPHA = 0x1,     /* has an alpha channel */
    GFX_PF_FOURCC = 0x4,    /* compressed, see fourCC */
    GFX_PF_RGB = 0x40,
    GFX_PF_BUMP = 0x40000   /* dU/dV/luminance */
};

/* Layout of a format, for the game's pixel code (DDPIXELFORMAT's flags). */
typedef struct GfxPixelFormat {
    DWORD flags;
    DWORD bitsPerPixel;
    DWORD redMask, greenMask, blueMask, alphaMask;   /* bump: dU, dV, luminance masks */
    DWORD fourCC;                                     /* compressed formats: 'DXT1', 'DXT5' */
} GfxPixelFormat;
void Gfx_GetPixelFormat(int format, GfxPixelFormat *out);

enum GfxTextureFlags {
    GFX_TEXTURE_RENDER_TARGET = 1,    /* can be drawn into (with its own depth buffer) */
    GFX_TEXTURE_CUBE = 2,             /* six faces */
    GFX_TEXTURE_DYNAMIC = 4,          /* changed often */
    GFX_TEXTURE_STAGING = 8,          /* CPU only: a source for Gfx_CopyTexture */
    GFX_TEXTURE_DISTANCE_FIELD = 16,  /* alpha is a signed distance to the edge, 0.5 on it: drawn with
                                         an edge of one pixel at any scale */
    GFX_TEXTURE_MENU_DOT = 32,       /* the original large menu dot, drawn as a circle when enlarged */
    GFX_TEXTURE_VECTOR_ICON = 64     /* traced colour/alpha fields, with native source bound separately */
};

/* mipLevels counts the top level (1 = no mipmaps). */
GfxTexture *Gfx_CreateTexture(int width, int height, int format, int mipLevels, DWORD flags);
void Gfx_DestroyTexture(GfxTexture *texture);
int Gfx_GetTextureMipCount(GfxTexture *texture);

typedef struct GfxLockedRect {
    void *pixels;
    LONG pitch;               /* bytes per row (per row of 4x4 blocks if compressed) */
    DWORD width, height;      /* of the mip level */
    GfxPixelFormat format;
} GfxLockedRect;

/* Locks a mip level of a face for reading and writing; the texture is
   updated on the GPU when it is unlocked. */
BOOL Gfx_LockTexture(GfxTexture *texture, int face, int mip, GfxLockedRect *out);
void Gfx_UnlockTexture(GfxTexture *texture, int face, int mip);
/* As Gfx_UnlockTexture, for a caller that changed only the pixels x0..x1-1,
   y0..y1-1 of level 0 of face 0 (the dirt stamps of the game on a car): the
   renderer can update that part alone. */
void Gfx_UnlockTextureRect(GfxTexture *texture, int x0, int y0, int x1, int y1);

/* Draws replacement wherever texture is drawn (NULL: texture itself), with
   the same texture coordinates; it is destroyed with texture. */
void Gfx_SetTextureReplacement(GfxTexture *texture, GfxTexture *replacement);
/* Texels of the distance field texture is drawn from (its replacement) per
   texel of its own, or 0 if it is drawn from its own pixels. */
int Gfx_GetDistanceFieldScale(GfxTexture *texture);
/* Give the large menu matrix dot a geometric replacement. Its original
   pixels are kept for native-size and smaller draws; the game still queues
   the same dots, colours and positions. */
void Gfx_VectorizeMenuDot(GfxTexture *texture);
/* Half a source texel for bitmaps, half a field texel for distance fields,
   and zero for geometry (which must not be cropped to texel centres). */
float Gfx_GetSpriteTexelInset(GfxTexture *texture);

/* Fills the lower mip levels from the top one (box filter). */
void Gfx_GenerateMipmaps(GfxTexture *texture);
/* Copies the top level of src into dst, converting the format (compressed
   sources are decoded) and stretching to dst's size like a DirectDraw Blt. */
void Gfx_CopyTexture(GfxTexture *dst, GfxTexture *src);

/* The frames of a hardware video decoder as the picture of a texture, with
   no copy (the PS Vita). pixels: a frame of the decoder, the size of the
   texture: the luma plane, then the two chroma samples of each block of 2x2
   pixels together (V first when vFirst). block, blockBytes: the memory
   block that holds the frame. The renderer makes it readable for the GPU
   the first time. The texture then draws that memory. The renderer first
   draws each frame that it has. FALSE: the renderer cannot (the caller
   writes the picture into the texture). */
BOOL Gfx_SetTextureVideoFrame(GfxTexture *texture, const void *pixels, BOOL vFirst, void *block, DWORD blockBytes);
/* Returns when the renderer has drawn each frame given to it. The memory of
   a video frame that they drew can change then. */
void Gfx_FinishDrawing(void);
/* The renderer reads a memory block of video frames no longer. It draws
   each frame that it has first. */
void Gfx_ReleaseVideoMemory(void *block);

/* Cube maps: a texture for one face, usable as a render target. Face 0 is
   the cube itself; destroying a face does nothing. */
GfxTexture *Gfx_GetCubeFace(GfxTexture *cube, int face);

/* Asks for a copy of a rectangle of the back buffer at the end of this frame
   and returns, in pixels (width * height colours, 0xAARRGGBB, rows top to
   bottom), the copy taken at the end of the previous frame. Returns 0 when
   there is none yet for a rectangle of that size. */
BOOL Gfx_ReadBackBuffer(int x, int y, int width, int height, DWORD *pixels);

#ifdef __cplusplus
}
#endif

#endif
