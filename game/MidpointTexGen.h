// PORT: the rows of Gfx_SetTexCoord1Generation that give the second texture
// coordinates of Graphics_ProjectMeshVertexMidpoints (the car reflection):
//   m = (position + normal) / 2
//   u1 = (m . (t11, t21, t31) + 1) / 2
//   v1 = (1 - m . (t12, t22, t32)) / 2
// with t the view * world matrix of the game (FloatMatrix_Multiply(t, view,
// world)). tests/midpoint_tex_gen.cpp compares them with the game's formula.
#ifndef CMR2_MIDPOINT_TEX_GEN_H
#define CMR2_MIDPOINT_TEX_GEN_H

#include "port/gfx.h"

static inline void MidpointTexGen_Rows(const GfxMatrix *pTransform, float rows[8])
{
    rows[0] = pTransform->_11 * 0.5f;
    rows[1] = pTransform->_21 * 0.5f;
    rows[2] = pTransform->_31 * 0.5f;
    rows[3] = 0.5f;
    rows[4] = -pTransform->_12 * 0.5f;
    rows[5] = -pTransform->_22 * 0.5f;
    rows[6] = -pTransform->_32 * 0.5f;
    rows[7] = 0.5f;
}

#endif
