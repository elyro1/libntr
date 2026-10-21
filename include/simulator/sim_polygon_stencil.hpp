#ifndef LIBNTR_SIM_POLYGON_STENCIL_HPP
#define LIBNTR_SIM_POLYGON_STENCIL_HPP

#ifdef SDK_BUILD_NX
#include <glad/glad.h>
#else
#include <simulator/glad/glad.h>
#endif

inline void SIM_ApplyPolygonStencil(int fieldEffectMode, int polygonId)
{
    // Soft PC shadows have varying fragment alpha. DS polygon-ID rejection
    // lets the first shadow's faint edge punch a hole in a later shadow.
    // Blend their coverage normally, without reading or writing that mask.
    if (fieldEffectMode == 1 || fieldEffectMode == 23) {
        glDisable(GL_STENCIL_TEST);
    } else {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_NOTEQUAL, 0x40 | polygonId, 0x7f);
    }
}

#endif
