#include "nitro/fx/fx.h"
#include <pthread.h>
#include <SDL2/SDL.h>

#ifdef SDK_BUILD_NX
#include <glad/glad.h>
#else
#include <simulator/glad/glad.h>
#endif

#include <stdio.h>
#include <stdlib.h>

#include <nitro/gx/g3.h>
#include <nitro/hw/X86/ioreg.h>
#include <nitro/hw/X86/mmap_shared.h>

#include <simulator/drawmsg.h>
#include <simulator/queue.h>
#include <simulator/g3_handler.h>
#include <simulator/g3_draw.h>
#include <simulator/sim.h>

#ifdef SDK_TRACY_ENABLE
#include "tracy/TracyC.h"
#endif

//Constants
const G3SIM_Matrix_t G3SIM_IdentityMatrix = 
    {
        .nums = {
            { FX32_ONE, 0,        0,        0        },
            { 0,        FX32_ONE, 0,        0        },
            { 0,        0,        FX32_ONE, 0        },
            { 0,        0,        0,        FX32_ONE },
        },
    };


GXMtxMode s_curMtxMode = GX_MTXMODE_PROJECTION;
GXBegin s_primType = GX_BEGIN_TRIANGLES;

G3SIM_MatrixStack_t s_projectionStack = {0};
G3SIM_MatrixStack_t s_positionStack = {0};
G3SIM_MatrixStack_t s_vectorStack = {0};
G3SIM_MatrixStack_t s_textureStack = {0};

static G3SIM_Matrix_t currentProjectionMatrix = {0};
static G3SIM_Matrix_t currentPositionMatrix = {0};
static G3SIM_Matrix_t currentVectorMatrix = {0};
static G3SIM_Matrix_t currentTextureMatrix = {0};

static G3SIM_Matrix_t currentClipMatrix = {0};

static u8 s_G3LightColor [4][3];
static float s_G3LightVector[4][3];

u8 s_G3numInPoly = 0;
u8 s_G3drawPoly = 0;
u32 s_G3triStripTriNum = 0;
u32 s_G3quadStripTriNum = 0;

GLfloat s_g3NextTexCoordS;
GLfloat s_g3NextTexCoordT;

G3SIM_FxVtx_t s_g3PolygonVerts[4];

u8 s_g3CurColor[3] = {0};
u8 s_g3DiffuseColor[3] = {0};
u8 s_g3AmbientColor[3] = {0};
u8 s_g3SpecularColor[3] = {0};
u8 s_g3EmissionColor[3] = {0};
G3SIM_PolygonAttr_t s_curPolygonAttr;

static u8 s_swapBuffersCalledThisFrame;

G3SIM_TexImageParam_t s_texImageParam;
u32 s_texPlttBase;

extern u8 s_SIM_useWBuffer;

static void CommonDebugCommandHandling(draw_command_type_t aCommand);

static float GetVtxPosition(G3SIM_Vertex_t * vtx, int comp)
{
    switch(comp){
        case 0:
            return vtx->x;
        case 1:
            return vtx->y;
        case 2:
            return vtx->z;
        case 3:
            return vtx->w;
        default:
            return 0.0f;
    }
}


void ClipSegment(fx32* outbuf, fx32 * vin, fx32 * vout, int comp, s32 plane, BOOL attribs)
{
    s64 factor_num = (s32)vin[3] - (plane * (s32)vin[comp]);
    s32 factor_den = factor_num - ((s32)vout[3] - (plane*(s32)vout[comp]));

#define INTERPOLATE(var)  { outbuf[var] = (vin[var] + ((vout[var] - vin[var]) * factor_num) / factor_den); }

    if (comp != 0) INTERPOLATE(0);
    if (comp != 1) INTERPOLATE(1);
    if (comp != 2) INTERPOLATE(2);
    INTERPOLATE(3);
    outbuf[comp] = plane*outbuf[3];

    //if (attribs)
    //{
    //    INTERPOLATE(Color[0]);
    //    INTERPOLATE(Color[1]);
    //    INTERPOLATE(Color[2]);
//
    //    INTERPOLATE(TexCoords[0]);
    //    INTERPOLATE(TexCoords[1]);
    //}

    //outbuf->Clipped = true;

#undef INTERPOLATE
}



static int ClipAgainstPlane(fx32 verts[][4], int nVerts, int clipStart, int comp, BOOL attribs)
{
    // A box face can cross the viewport even when all its original corners
    // are offscreen. Keep the actual intersections for subsequent planes.
    fx32 temp[10][4];
    for(int plane = 1; plane >= -1; plane -= 2) {
        if(nVerts <= clipStart) {
            return clipStart;
        }
        memcpy(temp, verts, clipStart * sizeof(temp[0]));
        int count = clipStart;
        const fx32 *previous = verts[nVerts - 1];
        s64 previousDistance = (s64)previous[3] - (s64)plane * previous[comp];
        for(int i = clipStart; i < nVerts; i++) {
            const fx32 *current = verts[i];
            s64 currentDistance = (s64)current[3] - (s64)plane * current[comp];
            if((previousDistance >= 0) != (currentDistance >= 0)) {
                double fraction = (double)previousDistance / (previousDistance - currentDistance);
                for(int axis = 0; axis < 4; axis++) {
                    temp[count][axis] = (fx32)(previous[axis] +
                        ((s64)current[axis] - previous[axis]) * fraction);
                }
                temp[count][comp] = plane * temp[count][3];
                count++;
            }
            if(currentDistance >= 0) {
                memcpy(temp[count++], current, sizeof(temp[0]));
            }
            previous = current;
            previousDistance = currentDistance;
        }
        memcpy(verts, temp, count * sizeof(temp[0]));
        nVerts = count;
    }
    return nVerts;
}

int G3SIM_ClipPolygon(fx32 verts[][4], int nVerts, int clipStart, BOOL attribs)
{
    // Z clipping
    nVerts = ClipAgainstPlane(verts, nVerts, clipStart, 2, attribs);

    // Y clipping
    nVerts = ClipAgainstPlane(verts, nVerts, clipStart, 1, attribs);

    // X clipping
    nVerts = ClipAgainstPlane(verts, nVerts, clipStart, 0, attribs);
    return nVerts;
}

static G3SIM_Matrix_t * getCurrentMatrix()
{
    switch( s_curMtxMode )
    {
        case GX_MTXMODE_PROJECTION:
            return &currentProjectionMatrix;
            break;
        case GX_MTXMODE_POSITION:
            return &currentPositionMatrix;
            break;
        case GX_MTXMODE_POSITION_VECTOR:
            return &currentVectorMatrix;
            break;
        case GX_MTXMODE_TEXTURE:
            return &currentTextureMatrix;
            break;
        default:
            return NULL;
    }
}

//Return a pointer to the top matrix from the current matrix stack
static G3SIM_Matrix_t * getTopMatrixFromCurrentStack()
{
    switch( s_curMtxMode )
    {
        case GX_MTXMODE_PROJECTION:
            return &s_projectionStack.mtxs[0];
            break;
        case GX_MTXMODE_POSITION:
            return &s_positionStack.mtxs[s_positionStack.stackPtr];
            break;
        case GX_MTXMODE_POSITION_VECTOR:
            return &s_vectorStack.mtxs[s_vectorStack.stackPtr];
            break;
        case GX_MTXMODE_TEXTURE:
            return &s_textureStack.mtxs[0];
            break;
        default:
            return NULL;
    }
}

static G3SIM_Matrix_t * getPositionMatrix()
{
    return &s_positionStack.mtxs[s_positionStack.stackPtr];
}

static G3SIM_Matrix_t * getVectorMatrix()
{
    return &s_vectorStack.mtxs[s_vectorStack.stackPtr];
}

//Get a pointer to the current matrix stack structure
static G3SIM_MatrixStack_t * getCurrentMatrixStack()
{
    switch( s_curMtxMode )
    {
        case GX_MTXMODE_PROJECTION:
            return &s_projectionStack;
            break;
        case GX_MTXMODE_POSITION:
            return &s_positionStack;
            break;
        case GX_MTXMODE_POSITION_VECTOR:
            return &s_vectorStack;
            break;
        case GX_MTXMODE_TEXTURE:
            return &s_textureStack;
            break;
        default:
            return NULL;
    }
}

static G3SIM_MatrixStack_t * getPositionMatrixStack()
{
    return &s_positionStack;
}

static G3SIM_MatrixStack_t * getVectorMatrixStack()
{
    return &s_vectorStack;
}

//Calculate the clip matrix
//Multiply the PositionMatrix * ProjectionMatrix
static void calculateClipMatrix()
{
    fx32 curSum = 0;
    //A * B
    for( int i = 0; i < 4; i++ ) //Row of A
    {
        for( int j = 0; j < 4; j++ )
        {
            
            //Get the dot product of curRow * curCol
            curSum = 0;
            for(int k=0; k < 4; k++)
            {
                curSum = curSum + FX_Mul(currentPositionMatrix.nums[i][k], currentProjectionMatrix.nums[k][j]);
            }
            currentClipMatrix.nums[i][j] = curSum;
        }
    }

    //Update reg_G3X_CLIPMTX_RESULT
    MtxFx44 fxMtx;
    fxMtx._00 = currentClipMatrix.nums[0][0];
    fxMtx._01 = currentClipMatrix.nums[0][1];
    fxMtx._02 = currentClipMatrix.nums[0][2];
    fxMtx._03 = currentClipMatrix.nums[0][3];
    fxMtx._10 = currentClipMatrix.nums[1][0];
    fxMtx._11 = currentClipMatrix.nums[1][1];
    fxMtx._12 = currentClipMatrix.nums[1][2];
    fxMtx._13 = currentClipMatrix.nums[1][3];
    fxMtx._20 = currentClipMatrix.nums[2][0];
    fxMtx._21 = currentClipMatrix.nums[2][1];
    fxMtx._22 = currentClipMatrix.nums[2][2];
    fxMtx._23 = currentClipMatrix.nums[2][3];
    fxMtx._30 = currentClipMatrix.nums[3][0];
    fxMtx._31 = currentClipMatrix.nums[3][1];
    fxMtx._32 = currentClipMatrix.nums[3][2];
    fxMtx._33 = currentClipMatrix.nums[3][3];

    memcpy( (void*)&s_reg_G3X_CLIPMTX_RESULT_0, &fxMtx, sizeof( MtxFx44 ) );

    // The vector matrix's 3x3 part, which G3X_GetVectorMtx reads back: NitroSystem's envelope (NODEMIX) models
    // read it to blend their joints' normal matrices
    MtxFx33 vecMtx;
    vecMtx._00 = currentVectorMatrix.nums[0][0];
    vecMtx._01 = currentVectorMatrix.nums[0][1];
    vecMtx._02 = currentVectorMatrix.nums[0][2];
    vecMtx._10 = currentVectorMatrix.nums[1][0];
    vecMtx._11 = currentVectorMatrix.nums[1][1];
    vecMtx._12 = currentVectorMatrix.nums[1][2];
    vecMtx._20 = currentVectorMatrix.nums[2][0];
    vecMtx._21 = currentVectorMatrix.nums[2][1];
    vecMtx._22 = currentVectorMatrix.nums[2][2];
    memcpy( (void*)&s_reg_G3X_VECMTX_RESULT_0, &vecMtx, sizeof( MtxFx33 ) );

    return;
}

static void calculateVtxFx(fx32 * x, fx32 * y, fx32 * z, fx32 * w)
{
    fx32 myW = FX32_ONE;
    fx32 oldVec[4];
    fx32 newVec[4];
    fx32 curSum;

    oldVec[0] = *x;
    oldVec[1] = *y;
    oldVec[2] = *z;
    oldVec[3] = FX32_ONE;

    for( int i=0; i < 4; i++ )
    {
        curSum = 0;
        for(int j=0; j < 4; j++ )
        {
            curSum = curSum + FX_Mul(oldVec[j], currentClipMatrix.nums[j][i]);
        }
        newVec[i] = curSum;
    }
    *x = newVec[0];
    *y = newVec[1];
    *z = newVec[2];
    *w = newVec[3];
}

// Project rendered vertices in two stages. Quantizing position * projection
// first makes coplanar foliage disagree across translated map chunks (notably
// Veilstone's altitude-2 forest). Keep the hardware clip matrix for box tests
// and register readback, but round each vertex dot product only once.
static void calculateRenderVtxFx(fx32 *x, fx32 *y, fx32 *z, fx32 *w)
{
    fx32 input[4] = {*x, *y, *z, FX32_ONE};
    const G3SIM_Matrix_t *matrices[2] = {&currentPositionMatrix, &currentProjectionMatrix};
    for(const G3SIM_Matrix_t *matrix : matrices) {
        fx32 output[4];
        for(int i = 0; i < 4; i++) {
            s64 sum = 0;
            for(int j = 0; j < 4; j++) {
                sum += (s64)input[j] * matrix->nums[j][i];
            }
            output[i] = (fx32)((sum + (1 << (FX32_SHIFT - 1))) >> FX32_SHIFT);
        }
        memcpy(input, output, sizeof(input));
    }
    *x = input[0];
    *y = input[1];
    *z = input[2];
    *w = input[3];
}

static void calculateVtx(float * x, float * y, float * z, float * w)
{
    float myW = 1.0f;

    float oldVec[4];
    float newVec[4];
    float curSum;

    oldVec[0] = *x;
    oldVec[1] = *y;
    oldVec[2] = *z;
    oldVec[3] = 1.0f;

    for( int i=0; i < 4; i++ )
    {
        curSum = 0.0f;
        for(int j=0; j < 4; j++ )
        {
            curSum = curSum + (oldVec[j] * FX_FX32_TO_F32(currentClipMatrix.nums[j][i]));
        }
        newVec[i] = curSum;
    }
    *x = newVec[0];
    *y = newVec[1];
    *z = newVec[2];
    *w = newVec[3];
    return;
}

//Begin vertices
void G3SIM_Begin(GXBegin type)
{
    char debugMessage[50] = {0};
    char * typeString = "";
    switch(type) {
        case GX_BEGIN_TRIANGLES:
            typeString = "SepTri";
            break;
        case GX_BEGIN_QUADS:
            typeString = "SepQuad";
            break;
        case GX_BEGIN_TRIANGLE_STRIP:
            typeString = "TriStrip";
            break;
        case GX_BEGIN_QUAD_STRIP:
            typeString = "QuadStrip";
            break;
    }
    snprintf(debugMessage, 50, "GX_Begin %s", typeString);

    glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_OTHER, 1337, GL_DEBUG_SEVERITY_NOTIFICATION, 50, debugMessage);
    s_primType = type;
    s_G3numInPoly = 0;
    s_G3drawPoly = 0;
    s_G3triStripTriNum = 0;
    s_G3quadStripTriNum = 0;

    //printf("GXBegin %d\n", type);

    return;
}

//Box Test
void G3SIM_BoxTest(s16 x, s16 y, s16 z, s16 width, s16 height, s16 depth)
{
    fx32 cube[8][4];
    fx32 face[10][4];

    fx32 x0 = x;
    fx32 y0 = y;
    fx32 z0 = z;
    fx32 x1 = width;
    fx32 y1 = height;
    fx32 z1 = depth;

    int res = 0;

    x1 += x0;
    y1 += y0;
    z1 += z0;

    cube[0][0] = x0; cube[0][1] = y0; cube[0][2] = z0;
    cube[1][0] = x1; cube[1][1] = y0; cube[1][2] = z0;
    cube[2][0] = x1; cube[2][1] = y1; cube[2][2] = z0;
    cube[3][0] = x0; cube[3][1] = y1; cube[3][2] = z0;
    cube[4][0] = x0; cube[4][1] = y1; cube[4][2] = z1;
    cube[5][0] = x0; cube[5][1] = y0; cube[5][2] = z1;
    cube[6][0] = x1; cube[6][1] = y0; cube[6][2] = z1;
    cube[7][0] = x1; cube[7][1] = y1; cube[7][2] = z1;

    for(int i=0; i < 8; i++) {
        calculateVtxFx(&cube[i][0], &cube[i][1], &cube[i][2], &cube[i][3]);
    }

    #define SetupFace(_face0,_face1,_face2,_face3) \
        face[0][0] = cube[_face0][0]; face[0][1] = cube[_face0][1]; face[0][2] = cube[_face0][2]; face[0][3] = cube[_face0][3]; \
        face[1][0] = cube[_face1][0]; face[1][1] = cube[_face1][1]; face[1][2] = cube[_face1][2]; face[1][3] = cube[_face1][3]; \
        face[2][0] = cube[_face2][0]; face[2][1] = cube[_face2][1]; face[2][2] = cube[_face2][2]; face[2][3] = cube[_face2][3]; \
        face[3][0] = cube[_face3][0]; face[3][1] = cube[_face3][1]; face[3][2] = cube[_face3][2]; face[3][3] = cube[_face3][3];

    // front face (-Z)
    SetupFace(0, 1, 2, 3);
    res += G3SIM_ClipPolygon(face, 4, 0, FALSE);

    // back face (+Z)
    SetupFace(4, 5, 6, 7);
    res += G3SIM_ClipPolygon(face, 4, 0, FALSE);

    // left face (-X)
    SetupFace(0, 3, 4, 5);
    res += G3SIM_ClipPolygon(face, 4, 0, FALSE);

    // right face (+X)
    SetupFace(1, 2, 7, 6);
    res += G3SIM_ClipPolygon(face, 4, 0, FALSE);

    // bottom face (-Y)
    SetupFace(0, 1, 6, 5);
    res += G3SIM_ClipPolygon(face, 4, 0, FALSE);

    // top face (+Y)
    SetupFace(2, 3, 4, 7);
    res += G3SIM_ClipPolygon(face, 4, 0, FALSE);

    if(res > 0){
        s_reg_G3X_GXSTAT |= (1<<1);
        return;
    }
    
    s_reg_G3X_GXSTAT &= 0b11111111111111111111111111111101;
    return;
}

//Set the color
void G3SIM_Color(u16 color)
{
    u8 r;
    u8 g;
    u8 b;
    SIM_u16ToRGB(color, &r, &g, &b);

    s_g3CurColor[0] = r;
    s_g3CurColor[1] = g;
    s_g3CurColor[2] = b;

    return;
}

void G3SIM_DecodeTex4(u8* vramTex, u16* colorAddr, u8* out, u32 s, u32 t){
    u32 texOffset = 0;
    u8 color1;
    u8 color2;
    u8 color3;
    u8 color4;
    u16 palColor;

    u8 r;
    u8 g;
    u8 b;
    for(int i=0; i < s; i++)
    {
        for(int j=0; j < t / 4; j++)
        {
            color1 = (*vramTex) & 0b00000011;
			color2 = ((*vramTex) & 0b00001100) >> 2;
            color3 = ((*vramTex) & 0b00110000) >> 4;
            color4 = ((*vramTex) & 0b11000000) >> 6;

            palColor = colorAddr[color1];
            SIM_u16ToRGB( palColor, &r, &g, &b );
			out[texOffset] = r;
			texOffset++;
			out[texOffset] = g;
			texOffset++;
			out[texOffset] = b;
			texOffset++;
            if(s_texImageParam.color0 == 1 && color1 == 0)
            {
                out[texOffset] = 0;
            }else{
                out[texOffset] = 255;
            }
			texOffset++;

            palColor = colorAddr[color2];
            SIM_u16ToRGB( palColor, &r, &g, &b );
			out[texOffset] = r;
			texOffset++;
			out[texOffset] = g;
			texOffset++;
			out[texOffset] = b;
			texOffset++;
            if(s_texImageParam.color0 == 1 && color2 == 0)
            {
                out[texOffset] = 0;
            }else{
                out[texOffset] = 255;
            }
			texOffset++;

            palColor = colorAddr[color3];
            SIM_u16ToRGB( palColor, &r, &g, &b );
			out[texOffset] = r;
			texOffset++;
			out[texOffset] = g;
			texOffset++;
			out[texOffset] = b;
			texOffset++;
            if(s_texImageParam.color0 == 1 && color3 == 0)
            {
                out[texOffset] = 0;
            }else{
                out[texOffset] = 255;
            }
			texOffset++;

            palColor = colorAddr[color4];
            SIM_u16ToRGB( palColor, &r, &g, &b );
			out[texOffset] = r;
			texOffset++;
			out[texOffset] = g;
			texOffset++;
			out[texOffset] = b;
			texOffset++;
            if(s_texImageParam.color0 == 1 && color4 == 0)
            {
                out[texOffset] = 0;
            }else{
                out[texOffset] = 255;
            }
			texOffset++;

            vramTex++;
        }
    }
}

void G3SIM_DecodeTex16(u8* vramTex, u16* colorAddr, u8* out, u32 s, u32 t){
    //4 bit per pixel (16 colour)
	u32 texOffset = 0;
    if( vramTex == NULL )
    {
        //This probably should not happen
        return;
    }
	for( int i=0; i < s; i++ )
	{
		for( int j=0; j < t / 2; j++ )
		{
			u8 color1 = (*vramTex) & 0b00001111;
			u8 color2 = ((*vramTex) & 0b11110000) >> 4;
			u16 palColor1;
			u16 palColor2;
			palColor1 = colorAddr[color1];
			palColor2 = colorAddr[color2];

			u8 r;
			u8 g;
			u8 b;

			SIM_u16ToRGB( palColor1, &r, &g, &b );
			out[texOffset] = r;
			texOffset++;
			out[texOffset] = g;
			texOffset++;
			out[texOffset] = b;
			texOffset++;
            if(s_texImageParam.color0 == 1 && color1 == 0)
            {
                out[texOffset] = 0;
            }else{
                out[texOffset] = 255;
            }
			texOffset++;

			//Color 2
			SIM_u16ToRGB( palColor2, &r, &g, &b );
			out[texOffset] = r;
			texOffset++;
			out[texOffset] = g;
			texOffset++;
			out[texOffset] = b;
			texOffset++;
            if(s_texImageParam.color0 == 1 && color2 == 0)
            {
                out[texOffset] = 0;
            }else{
                out[texOffset] = 255;
            }
			texOffset++;
			vramTex++;
		}
	}
}

void G3SIM_DecodeTex256(u8* vramTex, u16* colorAddr, u8* out, u32 s, u32 t){
    //4 bit per pixel (16 colour)
	u32 texOffset = 0;
	for( int i=0; i < s; i++ )
	{
		for( int j=0; j < t; j++ )
		{
			u8 color = (*vramTex);
			u16 palColor;
			palColor = colorAddr[color];

			u8 r;
			u8 g;
			u8 b;

			SIM_u16ToRGB( palColor, &r, &g, &b );
			out[texOffset] = r;
			texOffset++;
			out[texOffset] = g;
			texOffset++;
			out[texOffset] = b;
			texOffset++;
            if(s_texImageParam.color0 == 1 && color == 0)
            {
                out[texOffset] = 0;
            }else{
                out[texOffset] = 255;
            }
			texOffset++;
			vramTex++;
		}
	}
}

void G3SIM_DecodeTexA3I5(u8* vramTex, u16* colorAddr, u8* out, u32 s, u32 t)
{
    #ifdef SDK_TRACY_ENABLE
    TracyCZone(ctx, 1);
    #endif
    u32 texOffset = 0;
    u8 colorIdx;
    u8 alpha;

    u8 r;
    u8 g;
    u8 b;

    u16 color;
    for(int i=0; i < s; i++)
    {
        for(int j=0; j < t; j++)
        {
            colorIdx = (*vramTex) & 0b00011111;
            alpha = ((*vramTex) & 0b11100000) >> 5;
            //See GBATek A3I5
            alpha = (alpha*4) + (alpha/2);
            color = colorAddr[colorIdx];
            SIM_u16ToRGB( color, &r, &g, &b );
			out[texOffset] = r;
			texOffset++;
			out[texOffset] = g;
			texOffset++;
			out[texOffset] = b;
			texOffset++;
			out[texOffset] = alpha * 255 / 31;
			texOffset++;
            vramTex++;
        }
    }
    #ifdef SDK_TRACY_ENABLE
    TracyCZoneEnd(ctx);
    #endif
}

void G3SIM_DecodeTexA5I3(u8* vramTex, u16* colorAddr, u8* out, u32 s, u32 t)
{
    #ifdef SDK_TRACY_ENABLE
    TracyCZone(ctx, 1);
    #endif
    u32 texOffset = 0;
    u8 colorIdx;
    u8 alpha;

    u8 r;
    u8 g;
    u8 b;

    u16 color;
    for(int i=0; i < s; i++)
    {
        for(int j=0; j < t; j++)
        {
            colorIdx = (*vramTex) & 0b00000111;
            alpha = ((*vramTex) & 0b11111000) >> 3;
            color = colorAddr[colorIdx];
            SIM_u16ToRGB( color, &r, &g, &b );
			out[texOffset] = r;
			texOffset++;
			out[texOffset] = g;
			texOffset++;
			out[texOffset] = b;
			texOffset++;
			out[texOffset] = alpha * 255 / 31;
			texOffset++;
            vramTex++;
        }
    }
    #ifdef SDK_TRACY_ENABLE
    TracyCZoneEnd(ctx);
    #endif
}

void G3SIM_DecodeTexDirect(u8* vramTex, u8* out, u32 s, u32 t)
{
    #ifdef SDK_TRACY_ENABLE
    TracyCZone(ctx, 1);
    #endif
    u16 * colorAddr = (u16*)vramTex;
    u8 r;
    u8 g;
    u8 b;
    u32 texOffset = 0;
    for(int i=0; i < s; i++)
    {
        for(int j = 0; j < t; j++ )
        {
            u16 color = *colorAddr;
            SIM_u16ToRGB( color, &r, &g, &b );
			out[texOffset] = r;
			texOffset++;
			out[texOffset] = g;
			texOffset++;
			out[texOffset] = b;
			texOffset++;
			out[texOffset] = 255;
			texOffset++;
            colorAddr++;
        }
    }
    #ifdef SDK_TRACY_ENABLE
    TracyCZoneEnd(ctx);
    #endif
}

void G3SIM_DiffAmb(u32 data)
{
    u8 diffuseRed = (data & 0b11111);
    u8 diffuseGreen = (data & 0b1111100000) >> 5;
    u8 diffuseBlue = (data & 0b111110000000000) >> 10;
    u8 setVtxColor = (data & 0b1000000000000000) >> 15;
    u8 ambientRed = (data & 0b111110000000000000000) >> 16;
    u8 ambientGreen = (data & 0b11111000000000000000000000) >> 21;
    u8 ambientBlue = (data & 0b1111100000000000000000000000000) >> 26;

    if(setVtxColor) {
        s_g3CurColor[0] = diffuseRed << 3;
        s_g3CurColor[1] = diffuseGreen << 3;
        s_g3CurColor[2] = diffuseBlue << 3;
    }
    s_g3DiffuseColor[0] = diffuseRed << 3;
    s_g3DiffuseColor[1] = diffuseGreen << 3;
    s_g3DiffuseColor[2] = diffuseBlue << 3;

    s_g3AmbientColor[0] = ambientRed << 3;
    s_g3AmbientColor[1] = ambientGreen << 3;
    s_g3AmbientColor[2] = ambientBlue << 3;
    return;
}

//Copy the identity matrix to the current matrix
void G3SIM_Identity()
{
    G3SIM_Matrix_t * curMtx = getCurrentMatrix();
    if( s_curMtxMode == GX_MTXMODE_POSITION_VECTOR )
    {
        //Also copy to position matrix
        memcpy( &currentPositionMatrix, &G3SIM_IdentityMatrix, sizeof(G3SIM_Matrix_t) );
    }
    memcpy( curMtx, &G3SIM_IdentityMatrix, sizeof(G3SIM_Matrix_t) );

    if( s_curMtxMode != GX_MTXMODE_TEXTURE)
    {
        calculateClipMatrix();
    }
    return;
}

void G3SIM_LightColor(u32 data)
{
    u8 lightRed = (data & 0b11111);
    u8 lightGreen = (data & 0b1111100000) >> 5;
    u8 lightBlue = (data & 0b111110000000000) >> 10;
    u8 lightNum = (data & 0b11000000000000000000000000000000) >> 30;

    // Should get us close but colors might not be 100% accurate
    s_G3LightColor[lightNum][0] = lightRed << 3;
    s_G3LightColor[lightNum][1] = lightGreen << 3;
    s_G3LightColor[lightNum][2] = lightBlue << 3;
}

void G3SIM_LightVector(u32 data)
{
    u8 lightVecX = (data & 0b1111111111);
    u8 lightVecXSign = (lightVecX & 0b1000000000) >> 9;
    lightVecX = lightVecX & 0b111111111;
    u8 lightVecY = (data & 0b11111111110000000000) >> 10;
    u8 lightVecYSign = (lightVecY & 0b1000000000) >> 9;
    lightVecY = lightVecY & 0b111111111;
    u8 lightVecZ = (data & 0b111111111100000000000000000000) >> 20;
    u8 lightVecZSign = (lightVecZ & 0b1000000000) >> 9;
    lightVecZ = lightVecZ & 0b111111111;
    u8 lightNum = (data & 0b11000000000000000000000000000000) >> 30;

    // Should get us a number from -1 to 1
    s_G3LightVector[lightNum][0] = (float)lightVecX / 511.0f;
    if(lightVecXSign) {
        s_G3LightVector[lightNum][0] = s_G3LightVector[lightNum][0] * -1.0f;
    }
    s_G3LightVector[lightNum][1] = (float)lightVecY / 511.0f;
    if(lightVecYSign) {
        s_G3LightVector[lightNum][1] = s_G3LightVector[lightNum][1] * -1.0f;
    }
    s_G3LightVector[lightNum][2] = (float)lightVecZ / 511.0f;
    if(lightVecZSign) {
        s_G3LightVector[lightNum][2] = s_G3LightVector[lightNum][2] * -1.0f;
    }
}

//Load Matrix
void G3SIM_MtxLoad43(MtxFx43 * mtx)
{
    G3SIM_Matrix_t * curMtx = getCurrentMatrix();
    fx32 num;
    int curNum = 0;
    for(int i=0; i < 4; i++)
    {
        for(int j=0; j<3; j++)
        {
            curMtx->nums[i][j] = mtx->m[i][j];
            if( s_curMtxMode == GX_MTXMODE_POSITION_VECTOR )
            {
                currentPositionMatrix.nums[i][j] = mtx->m[i][j];
            }
        }
        if( s_curMtxMode == GX_MTXMODE_POSITION_VECTOR )
        {
            if( i==3 )
            {
                currentPositionMatrix.nums[i][3] = FX32_ONE;
            }
            else
            {
                currentPositionMatrix.nums[i][3] = 0;
            }
            
        }
        if( i==3 )
        {
            curMtx->nums[i][3] = FX32_ONE;
        }
        else
        {
            curMtx->nums[i][3] = 0;
        }
        
    }
    if( s_curMtxMode != GX_MTXMODE_TEXTURE )
    {
        calculateClipMatrix();
    }
    return;
}

void G3SIM_MtxLoad44(MtxFx44 * mtx)
{
    G3SIM_Matrix_t * curMtx = getCurrentMatrix();
    fx32 num;
    int curNum = 0;
    for(int i=0; i < 4; i++)
    {
        for(int j=0; j<4; j++)
        {
            curMtx->nums[i][j] = mtx->m[i][j];
            if( s_curMtxMode == GX_MTXMODE_POSITION_VECTOR )
            {
                currentPositionMatrix.nums[i][j] = mtx->m[i][j];
            }
        }
    }
        if( s_curMtxMode != GX_MTXMODE_TEXTURE )
    {
        calculateClipMatrix();
    }
    return;
}

//Change matrix mode
void G3SIM_MtxMode(GXMtxMode mode)
{
    s_curMtxMode = mode;


    if( mode > GX_MTXMODE_TEXTURE )
    {
        printf( "Warning: Invalid matrix mode %d\n", s_curMtxMode );
    }
    return;
}

//CurrentMatrix = CurrentMatrix * mtxs
void G3SIM_MtxMult33(MtxFx33 * mtx)
{
    G3SIM_Matrix_t mtxTemp;
    G3SIM_Matrix_t * currentMatrix;
    G3SIM_Matrix_t currentMatrixCopy;
    fx32 curSum;
    currentMatrix = getCurrentMatrix();
    mtxTemp.nums[0][0] = mtx->_00;
    mtxTemp.nums[0][1] = mtx->_01;
    mtxTemp.nums[0][2] = mtx->_02;
    mtxTemp.nums[0][3] = 0;
    mtxTemp.nums[1][0] = mtx->_10;
    mtxTemp.nums[1][1] = mtx->_11;
    mtxTemp.nums[1][2] = mtx->_12;
    mtxTemp.nums[1][3] = 0;
    mtxTemp.nums[2][0] = mtx->_20;
    mtxTemp.nums[2][1] = mtx->_21;
    mtxTemp.nums[2][2] = mtx->_22;
    mtxTemp.nums[2][3] = 0;
    mtxTemp.nums[3][0] = 0;
    mtxTemp.nums[3][1] = 0;
    mtxTemp.nums[3][2] = 0;
    mtxTemp.nums[3][3] = FX32_ONE;
    memcpy( &currentMatrixCopy, currentMatrix, sizeof(G3SIM_Matrix_t));

    //Do the multiplication
    for( int i=0; i < 3; i++ )
    {
        for(int j=0; j < 3; j++)
        {
            curSum = 0;
            for( int k=0; k < 3; k++ )
            {
                curSum = curSum + (FX_Mul(mtxTemp.nums[i][k], currentMatrixCopy.nums[k][j]));
            }
            currentMatrix->nums[i][j] = curSum;
            if( s_curMtxMode == GX_MTXMODE_POSITION_VECTOR )
            {
                currentPositionMatrix.nums[i][j] = curSum;
            }
        }
    }

    if( s_curMtxMode != GX_MTXMODE_TEXTURE )
    {
        calculateClipMatrix();
    }

    return;
}

//CurrentMatrix = CurrentMatrix * mtxs
void G3SIM_MtxMult43(MtxFx43 * mtx)
{
    G3SIM_Matrix_t mtxTemp;
    G3SIM_Matrix_t * currentMatrix;
    G3SIM_Matrix_t currentMatrixCopy;
    fx32 curSum;
    currentMatrix = getCurrentMatrix();
    mtxTemp.nums[0][0] = mtx->_00;
    mtxTemp.nums[0][1] = mtx->_01;
    mtxTemp.nums[0][2] = mtx->_02;
    mtxTemp.nums[0][3] = 0;
    mtxTemp.nums[1][0] = mtx->_10;
    mtxTemp.nums[1][1] = mtx->_11;
    mtxTemp.nums[1][2] = mtx->_12;
    mtxTemp.nums[1][3] = 0;
    mtxTemp.nums[2][0] = mtx->_20;
    mtxTemp.nums[2][1] = mtx->_21;
    mtxTemp.nums[2][2] = mtx->_22;
    mtxTemp.nums[2][3] = 0;
    mtxTemp.nums[3][0] = mtx->_30;
    mtxTemp.nums[3][1] = mtx->_31;
    mtxTemp.nums[3][2] = mtx->_32;
    mtxTemp.nums[3][3] = FX32_ONE;
    memcpy( &currentMatrixCopy, currentMatrix, sizeof(G3SIM_Matrix_t));

    //Do the multiplication
    for( int i=0; i < 4; i++ )
    {
        for(int j=0; j < 3; j++)
        {
            curSum = 0;
            for( int k=0; k < 4; k++ )
            {
                curSum = curSum + FX_Mul(mtxTemp.nums[i][k], currentMatrixCopy.nums[k][j]);
            }
            currentMatrix->nums[i][j] = curSum;
            if( s_curMtxMode == GX_MTXMODE_POSITION_VECTOR )
            {
                currentPositionMatrix.nums[i][j] = curSum;
            }
        }
    }

    if( s_curMtxMode != GX_MTXMODE_TEXTURE )
    {
        calculateClipMatrix();
    }
    return;
}

void G3SIM_MtxMult44(MtxFx44 * mtx)
{

    G3SIM_Matrix_t mtxTemp;
    G3SIM_Matrix_t * currentMatrix;
    G3SIM_Matrix_t currentMatrixCopy;
    fx32 curSum;
    currentMatrix = getCurrentMatrix();
    mtxTemp.nums[0][0] = mtx->_00;
    mtxTemp.nums[0][1] = mtx->_01;
    mtxTemp.nums[0][2] = mtx->_02;
    mtxTemp.nums[0][3] = mtx->_03;
    mtxTemp.nums[1][0] = mtx->_10;
    mtxTemp.nums[1][1] = mtx->_11;
    mtxTemp.nums[1][2] = mtx->_12;
    mtxTemp.nums[1][3] = mtx->_13;
    mtxTemp.nums[2][0] = mtx->_20;
    mtxTemp.nums[2][1] = mtx->_21;
    mtxTemp.nums[2][2] = mtx->_22;
    mtxTemp.nums[2][3] = mtx->_23;
    mtxTemp.nums[3][0] = mtx->_30;
    mtxTemp.nums[3][1] = mtx->_31;
    mtxTemp.nums[3][2] = mtx->_32;
    mtxTemp.nums[3][3] = mtx->_33;
    memcpy( &currentMatrixCopy, currentMatrix, sizeof(G3SIM_Matrix_t));

    //Do the multiplication
    for( int i=0; i < 4; i++ )
    {
        for(int j=0; j < 4; j++)
        {
            curSum = 0;
            for( int k=0; k < 4; k++ )
            {
                curSum = curSum + FX_Mul(mtxTemp.nums[i][k], currentMatrixCopy.nums[k][j]);
            }
            currentMatrix->nums[i][j] = curSum;
            if( s_curMtxMode == GX_MTXMODE_POSITION_VECTOR )
            {
                currentPositionMatrix.nums[i][j] = curSum;
            }
        }
    }

    if( s_curMtxMode != GX_MTXMODE_TEXTURE )
    {
        calculateClipMatrix();
    }
    return;
}

//TODO: Keep G3X_GXSTAT up to date
void G3SIM_MtxPop(u8 num)
{
    G3SIM_MatrixStack_t * curMtxStack = getCurrentMatrixStack();
    G3SIM_Matrix_t * currentMatrix = getCurrentMatrix();
    if( s_curMtxMode == GX_MTXMODE_POSITION_VECTOR
     || s_curMtxMode == GX_MTXMODE_POSITION )
    {
        //Perform stack operations on both position and vector matrices
        G3SIM_MatrixStack_t * positionMtxStack = getPositionMatrixStack();
        G3SIM_MatrixStack_t * vectorMtxStack = getVectorMatrixStack();
        if( num <= positionMtxStack->stackPtr)
        {
            positionMtxStack->stackPtr -= num;
        }
        else
        {
            positionMtxStack->stackPtr = 0;
        }

        if( num <= vectorMtxStack->stackPtr)
        {
            vectorMtxStack->stackPtr -= num;
        }
        else
        {
            vectorMtxStack->stackPtr = 0;
        }
        
        //Copy to current position and vector matrix
        memcpy( &currentPositionMatrix, &curMtxStack->mtxs[curMtxStack->stackPtr], sizeof(G3SIM_Matrix_t));
        memcpy( &currentVectorMatrix, &curMtxStack->mtxs[curMtxStack->stackPtr], sizeof(G3SIM_Matrix_t));
        calculateClipMatrix();
    }
    else
    {
        if( num <= curMtxStack->stackPtr )
        {
            curMtxStack->stackPtr -= num;
        }  
        else
        {
            curMtxStack->stackPtr = 0;
        }
        

        //Copy to current matrix
        memcpy( currentMatrix, &curMtxStack->mtxs[curMtxStack->stackPtr], sizeof(G3SIM_Matrix_t));
        if( s_curMtxMode == GX_MTXMODE_PROJECTION )
        {
            calculateClipMatrix();
        }
    }
    
    return;
}

void G3SIM_MtxPush()
{
    if( s_curMtxMode == GX_MTXMODE_POSITION_VECTOR
     || s_curMtxMode == GX_MTXMODE_POSITION )
    {
        //Position and vector
        G3SIM_MatrixStack_t * positionMtxStack = getPositionMatrixStack();
        G3SIM_MatrixStack_t * vectorMtxStack = getVectorMatrixStack();
        memcpy( &positionMtxStack->mtxs[positionMtxStack->stackPtr], &currentPositionMatrix, sizeof(G3SIM_Matrix_t) );
        memcpy( &vectorMtxStack->mtxs[vectorMtxStack->stackPtr], &currentVectorMatrix, sizeof(G3SIM_Matrix_t) );
        positionMtxStack->stackPtr++;
        vectorMtxStack->stackPtr++;
    }
    else
    {
        G3SIM_MatrixStack_t * curMtxStack = getCurrentMatrixStack();
        G3SIM_Matrix_t * currentMatrix = getCurrentMatrix();
        memcpy( &curMtxStack->mtxs[curMtxStack->stackPtr], currentMatrix, sizeof(G3SIM_Matrix_t) );
        curMtxStack->stackPtr++;
    }
    return;
}

void G3SIM_MtxRestore(u8 idx)
{
    G3SIM_MatrixStack_t * curMtxStack = getCurrentMatrixStack();
    G3SIM_Matrix_t * currentMatrix = getCurrentMatrix();
    if( s_curMtxMode == GX_MTXMODE_POSITION_VECTOR
     || s_curMtxMode == GX_MTXMODE_POSITION )
    {
        //Restore to both current position matrix and current vector matrix
        G3SIM_MatrixStack_t * positionMtxStack = getPositionMatrixStack();
        G3SIM_MatrixStack_t * vectorMtxStack = getVectorMatrixStack();
        memcpy( &currentPositionMatrix, &positionMtxStack->mtxs[idx], sizeof(G3SIM_Matrix_t) );
        memcpy( &currentVectorMatrix, &vectorMtxStack->mtxs[idx], sizeof(G3SIM_Matrix_t) );
        calculateClipMatrix();
    }
    else
    {
        memcpy( currentMatrix, &curMtxStack->mtxs[idx], sizeof(G3SIM_Matrix_t) );
        if( s_curMtxMode == GX_MTXMODE_PROJECTION )
        {
            calculateClipMatrix();
        }
    }


    return;
}

void G3SIM_MtxScale(fx32 * scale)
{
    fx32 s[3];

    s[0] = scale[0];
    s[1] = scale[1];
    s[2] = scale[2];
    
    G3SIM_Matrix_t * currentMatrix = getCurrentMatrix();

    for(int i=0; i < 3; i ++)
    {
        for( int j=0; j < 3; j ++)
        {
            currentMatrix->nums[i][j] = FX_Mul(s[i], currentMatrix->nums[i][j]);
            if( s_curMtxMode == GX_MTXMODE_POSITION_VECTOR )
            {
                //Also multiply the position matrix
                currentPositionMatrix.nums[i][j] = FX_Mul(s[i], currentPositionMatrix.nums[i][j]);
            }
        }   
    }
    if( s_curMtxMode != GX_MTXMODE_TEXTURE )
    {
        calculateClipMatrix();
    }
    return;
}

void G3SIM_MtxStore(u8 idx)
{
    if( s_curMtxMode == GX_MTXMODE_POSITION_VECTOR
     || s_curMtxMode == GX_MTXMODE_POSITION )
    {
        G3SIM_MatrixStack_t * positionMtxStack = getPositionMatrixStack();
        G3SIM_MatrixStack_t * vectorMtxStack = getVectorMatrixStack();
        memcpy( &positionMtxStack->mtxs[idx], &currentPositionMatrix, sizeof(G3SIM_Matrix_t) );
        memcpy( &vectorMtxStack->mtxs[idx], &currentVectorMatrix, sizeof(G3SIM_Matrix_t) );
    }
    else
    {
        G3SIM_MatrixStack_t * curMtxStack = getCurrentMatrixStack();
        G3SIM_Matrix_t * currentMatrix = getCurrentMatrix();
        memcpy( &curMtxStack->mtxs[idx], currentMatrix, sizeof(G3SIM_Matrix_t) );
    }


    return;
}



void G3SIM_MtxTranslate(fx32* trans)
{
    fx32 s[3];

    s[0] = trans[0];
    s[1] = trans[1];
    s[2] = trans[2];
    G3SIM_Matrix_t * matrix = getCurrentMatrix();
    if( s_curMtxMode == GX_MTXMODE_POSITION_VECTOR )
    {
        currentPositionMatrix.nums[3][0] += (FX_Mul(s[0],currentPositionMatrix.nums[0][0]) + FX_Mul(s[1],currentPositionMatrix.nums[1][0]) + FX_Mul(s[2],currentPositionMatrix.nums[2][0]));
        currentPositionMatrix.nums[3][1] += (FX_Mul(s[0],currentPositionMatrix.nums[0][1]) + FX_Mul(s[1],currentPositionMatrix.nums[1][1]) + FX_Mul(s[2],currentPositionMatrix.nums[2][1]));
        currentPositionMatrix.nums[3][2] += (FX_Mul(s[0],currentPositionMatrix.nums[0][2]) + FX_Mul(s[1],currentPositionMatrix.nums[1][2]) + FX_Mul(s[2],currentPositionMatrix.nums[2][2]));
        currentPositionMatrix.nums[3][3] += (FX_Mul(s[0],currentPositionMatrix.nums[0][3]) + FX_Mul(s[1],currentPositionMatrix.nums[1][3]) + FX_Mul(s[2],currentPositionMatrix.nums[2][3]));
    }
    matrix->nums[3][0] += (FX_Mul(s[0],matrix->nums[0][0]) + FX_Mul(s[1],matrix->nums[1][0]) + FX_Mul(s[2],matrix->nums[2][0]));
    matrix->nums[3][1] += (FX_Mul(s[0],matrix->nums[0][1]) + FX_Mul(s[1],matrix->nums[1][1]) + FX_Mul(s[2],matrix->nums[2][1]));
    matrix->nums[3][2] += (FX_Mul(s[0],matrix->nums[0][2]) + FX_Mul(s[1],matrix->nums[1][2]) + FX_Mul(s[2],matrix->nums[2][2]));
    matrix->nums[3][3] += (FX_Mul(s[0],matrix->nums[0][3]) + FX_Mul(s[1],matrix->nums[1][3]) + FX_Mul(s[2],matrix->nums[2][3]));

    if( s_curMtxMode != GX_MTXMODE_TEXTURE )
    {
        calculateClipMatrix();
    }
}

void G3SIM_Normal(u32 data)
{
    u8 vecX = (data & 0b1111111111);
    u8 vecXSign = (vecX & 0b1000000000) >> 9;
    vecX = vecX & 0b111111111;
    u8 vecY = (data & 0b11111111110000000000) >> 10;
    u8 vecYSign = (vecY & 0b1000000000) >> 9;
    vecY = vecY & 0b111111111;
    u8 vecZ = (data & 0b111111111100000000000000000000) >> 20;
    u8 vecZSign = (vecZ & 0b1000000000) >> 9;
    vecZ = vecZ & 0b111111111;

    float normalVectorIn[3];
    // Should get us a number from -1 to 1
    normalVectorIn[0] = (float)vecX / 511.0f;
    if(vecXSign) {
        normalVectorIn[0] = normalVectorIn[0] * -1.0f;
    }
    normalVectorIn[1] = (float)vecY / 511.0f;
    if(vecYSign) {
        normalVectorIn[1] = normalVectorIn[1] * -1.0f;
    }
    normalVectorIn[2] = (float)vecZ / 511.0f;
    if(vecZSign) {
        normalVectorIn[2] = normalVectorIn[2] * -1.0f;
    }

    //Multiply by the vector matrix
    float normalVector[3];
    normalVector[0] = (normalVectorIn[0] * FX_FX32_TO_F32(currentVectorMatrix.nums[0][0]))
                    + (normalVectorIn[1] * FX_FX32_TO_F32(currentVectorMatrix.nums[1][0]))
                    + (normalVectorIn[2] * FX_FX32_TO_F32(currentVectorMatrix.nums[2][0]));
    
    normalVector[1] = (normalVectorIn[0] * FX_FX32_TO_F32(currentVectorMatrix.nums[0][1]))
                    + (normalVectorIn[1] * FX_FX32_TO_F32(currentVectorMatrix.nums[1][1]))
                    + (normalVectorIn[2] * FX_FX32_TO_F32(currentVectorMatrix.nums[2][1]));

    normalVector[2] = (normalVectorIn[0] * FX_FX32_TO_F32(currentVectorMatrix.nums[0][2]))
                    + (normalVectorIn[1] * FX_FX32_TO_F32(currentVectorMatrix.nums[1][2]))
                    + (normalVectorIn[2] * FX_FX32_TO_F32(currentVectorMatrix.nums[2][2]));

    // Calculate the color

    float color[3] = {};

    color[0] = (float)s_g3EmissionColor[0] / 255.0f;
    color[1] = (float)s_g3EmissionColor[1] / 255.0f;
    color[2] = (float)s_g3EmissionColor[2] / 255.0f;

    for(int lightNum=0; lightNum<4; lightNum++) {
        if((s_curPolygonAttr.lightFlag >> lightNum) & 1)
        {
            // Light is enabled

            //Multiply light vector by vector matrix
            float lightVector[3];
            lightVector[0] = (s_G3LightVector[lightNum][0] * FX_FX32_TO_F32(currentVectorMatrix.nums[0][0]))
                           + (s_G3LightVector[lightNum][1] * FX_FX32_TO_F32(currentVectorMatrix.nums[1][0]))
                           + (s_G3LightVector[lightNum][2] * FX_FX32_TO_F32(currentVectorMatrix.nums[2][0]));

            lightVector[1] = (s_G3LightVector[lightNum][0] * FX_FX32_TO_F32(currentVectorMatrix.nums[0][1]))
                           + (s_G3LightVector[lightNum][1] * FX_FX32_TO_F32(currentVectorMatrix.nums[1][1]))
                           + (s_G3LightVector[lightNum][2] * FX_FX32_TO_F32(currentVectorMatrix.nums[2][1]));

            lightVector[2] = (s_G3LightVector[lightNum][0] * FX_FX32_TO_F32(currentVectorMatrix.nums[0][2]))
                           + (s_G3LightVector[lightNum][1] * FX_FX32_TO_F32(currentVectorMatrix.nums[1][2]))
                           + (s_G3LightVector[lightNum][2] * FX_FX32_TO_F32(currentVectorMatrix.nums[2][2]));

            //Calculate Diffuse level
            float diffuseLevel = (lightVector[0] * normalVector[0])
                               + (lightVector[1] * normalVector[1])
                               + (lightVector[2] * normalVector[2]);

            diffuseLevel = diffuseLevel * -1.0f;
            if(diffuseLevel < 0.0f) {
                diffuseLevel = 0.0f;
            }

            //Specular color
            color[0] = color[0] + (((float)s_g3SpecularColor[0] / 255.0f) * ((float)s_G3LightColor[lightNum][0] / 255.0f)); // * ShininessLevel TODO
            color[1] = color[1] + (((float)s_g3SpecularColor[1] / 255.0f) * ((float)s_G3LightColor[lightNum][1] / 255.0f)); // * ShininessLevel TODO
            color[2] = color[2] + (((float)s_g3SpecularColor[2] / 255.0f) * ((float)s_G3LightColor[lightNum][2] / 255.0f)); // * ShininessLevel TODO

            //Diffuse color
            color[0] = color[0] + (((float)s_g3DiffuseColor[0] / 255.0f) * ((float)s_G3LightColor[lightNum][0] / 255.0f) * diffuseLevel); // * DiffuseLevel TODO
            color[1] = color[1] + (((float)s_g3DiffuseColor[1] / 255.0f) * ((float)s_G3LightColor[lightNum][1] / 255.0f) * diffuseLevel); // * DiffuseLevel TODO
            color[2] = color[2] + (((float)s_g3DiffuseColor[2] / 255.0f) * ((float)s_G3LightColor[lightNum][2] / 255.0f) * diffuseLevel); // * DiffuseLevel TODO

            //Ambient color
            color[0] = color[0] + (((float)s_g3AmbientColor[0] / 255.0f) * ((float)s_G3LightColor[lightNum][0] / 255.0f));
            color[1] = color[1] + (((float)s_g3AmbientColor[1] / 255.0f) * ((float)s_G3LightColor[lightNum][1] / 255.0f));
            color[2] = color[2] + (((float)s_g3AmbientColor[2] / 255.0f) * ((float)s_G3LightColor[lightNum][2] / 255.0f));
        }
    }

    if(color[0] > 1.0f) {
        color[0] = 1.0f;
    }
    if(color[1] > 1.0f) {
        color[1] = 1.0f;
    }
    if(color[2] > 1.0f) {
        color[2] = 1.0f;
    }

    s_g3CurColor[0] = (u8)(color[0] * 255.0f);
    s_g3CurColor[1] = (u8)(color[1] * 255.0f);
    s_g3CurColor[2] = (u8)(color[2] * 255.0f);
}

//PolygonAttr
void G3SIM_PolygonAttr(u32 data)
{
    u8 lightFlag = data & 0b1111;
    u8 alpha = (data & 0b111110000000000000000) >> 16;
    u8 polygonMode = (data & 0b110000) >> 4;
    u8 cullMode = (data & 0b11000000) >> 6;
    u8 depthTest = (data & 0b100000000000000) >> 14;
    u8 useFog = (data & 0b1000000000000000) >> 15;
    u8 translucentDepth = (data >> 11) & 1;
    u8 polygonId = (data >> 24) & 63;

    if(cullMode != s_curPolygonAttr.cullMode || polygonMode != s_curPolygonAttr.polygonMode || alpha != s_curPolygonAttr.alphaInt || depthTest != s_curPolygonAttr.depthTest || useFog != s_curPolygonAttr.fogEnable || translucentDepth != s_curPolygonAttr.translucentDepth || polygonId != s_curPolygonAttr.polygonId)
    {
        // Flush the vertex buffer
        G3SIM_FlushArray();
    }

    if(depthTest != s_curPolygonAttr.depthTest)
    {
        if(depthTest){
            glDepthFunc(GL_LEQUAL);
        } else {
            glDepthFunc(GL_LESS);
        }
    }

    s_curPolygonAttr.lightFlag = lightFlag;
    s_curPolygonAttr.cullMode = cullMode;
    s_curPolygonAttr.polygonMode = polygonMode;
    s_curPolygonAttr.alphaInt = alpha;
    s_curPolygonAttr.depthTest = depthTest;
    s_curPolygonAttr.fogEnable = useFog;
    s_curPolygonAttr.translucentDepth = translucentDepth;
    s_curPolygonAttr.polygonId = polygonId;

    if(alpha == 31){
        s_curPolygonAttr.alpha = 1.0f;
    } else {
        s_curPolygonAttr.alpha = (float)alpha / 31.0f;
    }

    switch(cullMode) {
        case GX_CULL_ALL:
            glEnable( GL_CULL_FACE );
            glCullFace(GL_FRONT_AND_BACK);
            break;
        case GX_CULL_FRONT:
            glEnable( GL_CULL_FACE );
            glCullFace(GL_BACK);
            break;
        case GX_CULL_BACK:
            glEnable( GL_CULL_FACE );
            glCullFace(GL_FRONT);
            break;
        case GX_CULL_NONE:
            glDisable( GL_CULL_FACE );
            break;
    }


    return;
}

void G3SIM_SpecEmi(u32 data)
{
    u8 specularRed = (data & 0b11111);
    u8 specularGreen = (data & 0b1111100000) >> 5;
    u8 specularBlue = (data & 0b111110000000000) >> 10;
    u8 setVtxColor = (data & 0b1000000000000000) >> 15;
    u8 emissionRed = (data & 0b111110000000000000000) >> 16;
    u8 emissionGreen = (data & 0b11111000000000000000000000) >> 21;
    u8 emissionBlue = (data & 0b1111100000000000000000000000000) >> 26;

    if(setVtxColor) {
        s_g3CurColor[0] = specularRed << 3;
        s_g3CurColor[1] = specularGreen << 3;
        s_g3CurColor[2] = specularBlue << 3;
    }
    s_g3SpecularColor[0] = specularRed << 3;
    s_g3SpecularColor[1] = specularGreen << 3;
    s_g3SpecularColor[2] = specularBlue << 3;

    s_g3EmissionColor[0] = emissionRed << 3;
    s_g3EmissionColor[1] = emissionGreen << 3;
    s_g3EmissionColor[2] = emissionBlue << 3;
}

//SwapBuffers
void G3SIM_SwapBuffers(u8 depthBufferMode)
{
    s_SIM_useWBuffer = depthBufferMode;
    s_swapBuffersCalledThisFrame = TRUE;
}

u8 G3SIM_SwapBuffersCalledThisFrame()
{
    return s_swapBuffersCalledThisFrame;
}

void G3SIM_ResetSwapBuffersCall()
{
    s_swapBuffersCalledThisFrame = FALSE;
}

//TexCoord
void G3SIM_TexCoord(u32 data)
{
    s16 s = data & 0xFFFF;
    s16 t = (data & 0xFFFF0000) >> 16;

    //Multiply by the texture matrix
    //See GBATek section on Texture Matrix
    if(s_texImageParam.texCordTransform == 1) {
        float texCoordVector[4];
        texCoordVector[0] = (float)s / 16.0f;
        texCoordVector[1] = (float)t / 16.0f;
        texCoordVector[2] = 1.0f / 16.0f;
        texCoordVector[3] = 1.0f / 16.0f;

        G3SIM_Matrix_t * textureMtx = &currentTextureMatrix;


        s_g3NextTexCoordS = (texCoordVector[0] * FX_FX32_TO_F32(textureMtx->nums[0][0]))
                          + (texCoordVector[1] * FX_FX32_TO_F32(textureMtx->nums[1][0]))
                          + (texCoordVector[2] * FX_FX32_TO_F32(textureMtx->nums[2][0]))
                          + (texCoordVector[3] * FX_FX32_TO_F32(textureMtx->nums[3][0]));

        s_g3NextTexCoordT = (texCoordVector[0] * FX_FX32_TO_F32(textureMtx->nums[0][1]))
                          + (texCoordVector[1] * FX_FX32_TO_F32(textureMtx->nums[1][1]))
                          + (texCoordVector[2] * FX_FX32_TO_F32(textureMtx->nums[2][1]))
                          + (texCoordVector[3] * FX_FX32_TO_F32(textureMtx->nums[3][1]));
    } else {
        s_g3NextTexCoordS = ((float)s)/16.0f;
        s_g3NextTexCoordT = ((float)t)/16.0f;
    }

    return;
}

//TexImageParam
void G3SIM_TexImageParam(u32 data)
{
#if defined(SDK_BUILD_LINUX) || defined(SDK_BUILD_WIN64)
    s_texImageParam.textureOffset = SIM_DecodeTextureAddress(&data);
#else
    s_texImageParam.textureOffset = ((u16)data) << 3;
#endif
    s_texImageParam.repeatS = (data & 0b10000000000000000) >> 16;
    s_texImageParam.repeatT = (data & 0b100000000000000000) >> 17;
    s_texImageParam.flipS = (data & 0b1000000000000000000) >> 18;
    s_texImageParam.flipT = (data & 0b10000000000000000000) >> 19;
    s_texImageParam.textureSSize = 8 << ((data & 0b11100000000000000000000) >> 20);
    s_texImageParam.textureTSize = 8 << ((data & 0b11100000000000000000000000) >> 23);
    s_texImageParam.textureFormat = (data & 0b11100000000000000000000000000) >> 26;
    s_texImageParam.color0 = (data & 0b100000000000000000000000000000) >> 29;
    s_texImageParam.texCordTransform = (data & 0b11000000000000000000000000000000) >> 30;
    return;
}

//TexPlttBase
void G3SIM_TexPlttBase(u32 data)
{
    if( s_texImageParam.textureFormat == 2 )
    {
        s_texPlttBase = data << 3;
    }
    else
    {
        s_texPlttBase = data << 4;
    }
    return;
}

//Add a vertex
void G3SIM_Vtx(s16 x, s16 y, s16 z)
{
    fx32 fxX;
    fx32 fxY;
    fx32 fxZ;
    fx32 fxW;

    fxX = x;
    fxY = y;
    fxZ = z;
    fxW = FX32_ONE;

    calculateRenderVtxFx(&fxX, &fxY, &fxZ, &fxW);

    s_g3PolygonVerts[s_G3numInPoly].x = fxX;
    s_g3PolygonVerts[s_G3numInPoly].y = fxY;
    s_g3PolygonVerts[s_G3numInPoly].z = fxZ;
    s_g3PolygonVerts[s_G3numInPoly].w = fxW;

    s_g3PolygonVerts[s_G3numInPoly].r = (float)s_g3CurColor[0] / 255.0f;
    s_g3PolygonVerts[s_G3numInPoly].g = (float)s_g3CurColor[1] / 255.0f;
    s_g3PolygonVerts[s_G3numInPoly].b = (float)s_g3CurColor[2] / 255.0f;
    s_g3PolygonVerts[s_G3numInPoly].a = s_curPolygonAttr.alpha;

    s_g3PolygonVerts[s_G3numInPoly].s = s_g3NextTexCoordS / (float)s_texImageParam.textureSSize;
    s_g3PolygonVerts[s_G3numInPoly].t = s_g3NextTexCoordT / (float)s_texImageParam.textureTSize;

    s_G3numInPoly++;
    if( s_primType == GX_BEGIN_QUADS )
    {
        if( s_G3numInPoly >= 4 )
        {
            s_G3numInPoly = 0;
        }       
    }
    if( s_primType == GX_BEGIN_TRIANGLES )
    {
        if( s_G3numInPoly >= 3 )
        {
            s_G3numInPoly = 0;
        }
    }
    if( s_primType == GX_BEGIN_QUAD_STRIP )
    {        
        if( s_G3numInPoly >= 4 )
        {
            s_G3numInPoly = 2;
            s_G3drawPoly = 1;
            s_G3quadStripTriNum++;
        }
    }
    if( s_primType == GX_BEGIN_TRIANGLE_STRIP )
    {
        if( s_G3numInPoly >= 3 )
        {
            s_G3numInPoly = 2;
            s_G3drawPoly = 1;
            s_G3triStripTriNum++;
        }
    }


    return;
}

// Clip before perspective division and depth quantization. In particular, a
// negative W must not be masked into a positive value for geometry behind the
// camera, such as the long Distortion World waterfall.
static int ClipRenderPolygon(G3SIM_FxVtx_t *verts, int count, bool nearPlane)
{
    G3SIM_FxVtx_t clipped[8];
    int outCount = 0;
    auto distance = [nearPlane](const G3SIM_FxVtx_t &v) -> s64 {
        return nearPlane ? (s64)v.z + v.w : (s64)v.w - 1;
    };
    G3SIM_FxVtx_t previous = verts[count - 1];
    s64 previousDistance = distance(previous);
    for(int i = 0; i < count; i++) {
        const G3SIM_FxVtx_t &current = verts[i];
        s64 currentDistance = distance(current);
        if((previousDistance >= 0) != (currentDistance >= 0)) {
            double fraction = (double)previousDistance / (previousDistance - currentDistance);
            G3SIM_FxVtx_t intersection;
            for(int axis = 0; axis < 4; axis++) {
                intersection.nums[axis] = (fx32)(previous.nums[axis] +
                    ((s64)current.nums[axis] - previous.nums[axis]) * fraction);
            }
            // Keep the intersection exactly on the plane after rounding.
            if(nearPlane) {
                intersection.z = -intersection.w;
            } else {
                intersection.w = 1;
            }
#define INTERPOLATE_ATTRIBUTE(attribute) \
            intersection.attribute = previous.attribute + \
                (current.attribute - previous.attribute) * fraction
            INTERPOLATE_ATTRIBUTE(s);
            INTERPOLATE_ATTRIBUTE(t);
            INTERPOLATE_ATTRIBUTE(r);
            INTERPOLATE_ATTRIBUTE(g);
            INTERPOLATE_ATTRIBUTE(b);
            INTERPOLATE_ATTRIBUTE(a);
#undef INTERPOLATE_ATTRIBUTE
            clipped[outCount++] = intersection;
        }
        if(currentDistance >= 0) {
            clipped[outCount++] = current;
        }
        previous = current;
        previousDistance = currentDistance;
    }
    memcpy(verts, clipped, outCount * sizeof(*verts));
    return outCount;
}

void G3SIM_SubmitPolygon(G3SIM_FxVtx_t *fxVerts, int numVerts)
{
    if(numVerts < 3 || numVerts > 4) {
        SIM_assert_always();
        return;
    }

    // Work on a copy: strips reuse the source vertices for their next polygon.
    G3SIM_FxVtx_t clipped[8];
    memcpy(clipped, fxVerts, numVerts * sizeof(*fxVerts));
    numVerts = ClipRenderPolygon(clipped, numVerts, true);
    if(numVerts < 3) {
        return;
    }
    numVerts = ClipRenderPolygon(clipped, numVerts, false);
    if(numVerts < 3) {
        return;
    }

    G3SIM_Vertex_t glVerts[8];
    for(int i = 0; i < numVerts; i++) {
        const G3SIM_FxVtx_t &v = clipped[i];
        G3SIM_Vertex_t &out = glVerts[i];
        out.x = (float)v.x / v.w;
        out.y = (float)v.y / v.w;
        out.w = FX_FX32_TO_F32(v.w);
        out.s = v.s;
        out.t = v.t;
        out.r = v.r;
        out.g = v.g;
        out.b = v.b;
        out.a = v.a;

        if(!s_SIM_useWBuffer) {
            // Keep continuous vertex Z. Rounding Z/W to 15 bits before
            // interpolation erases the tiny gaps between map foliage faces.
            out.z = (float)((double)v.z / v.w);
            if(out.z < -1.0f) out.z = -1.0f;
            if(out.z > 1.0f) out.z = 1.0f;
            continue;
        }
        s64 z = v.w;
        // Preserve W; its depth is evaluated from the reciprocal-W plane.
        if(z < 0) {
            z = 0;
        } else if(z > 0xFFFFFF) {
            z = 0xFFFFFF;
        }
        out.z = ((float)z / 16777215.0f) * 2.0f - 1.0f;
    }

    // Clipping may turn a quad into a pentagon. Compute depth per triangle
    // so nonplanar quads retain the same triangulation as their visible mesh.
    for(int i = 1; i < numVerts - 1; i++) {
        // Solve the depth plane in clip space: reciprocal W uses a constant
        // RHS, while Z uses vertex clip Z. Pixel evaluation gives adjacent
        // coplanar faces the same depth in both buffer modes.
        float depthPlane[3] = {};
        const auto &p = clipped[0];
        const auto &q = clipped[i];
        const auto &r = clipped[i + 1];
        double a = (double)q.y * r.w - (double)q.w * r.y;
        double b = (double)q.w * r.x - (double)q.x * r.w;
        double c = (double)q.x * r.y - (double)q.y * r.x;
        double determinant = p.x * a + p.y * b + p.w * c;
        if(determinant != 0) {
            double dp = s_SIM_useWBuffer ? FX32_ONE : p.z;
            double dq = s_SIM_useWBuffer ? FX32_ONE : q.z;
            double dr = s_SIM_useWBuffer ? FX32_ONE : r.z;
            depthPlane[0] = (float)((dp*a + dq*((double)r.y*p.w - (double)r.w*p.y) +
                dr*((double)p.y*q.w - (double)p.w*q.y)) / determinant);
            depthPlane[1] = (float)((dp*b + dq*((double)r.w*p.x - (double)r.x*p.w) +
                dr*((double)p.w*q.x - (double)p.x*q.w)) / determinant);
            depthPlane[2] = (float)((dp*c + dq*((double)r.x*p.y - (double)r.y*p.x) +
                dr*((double)p.x*q.y - (double)p.y*q.x)) / determinant);
        }
        memcpy(glVerts[0].depthPlane, depthPlane, sizeof(depthPlane));
        memcpy(glVerts[i].depthPlane, depthPlane, sizeof(depthPlane));
        memcpy(glVerts[i + 1].depthPlane, depthPlane, sizeof(depthPlane));
        G3SIM_AddVtx(&glVerts[0]);
        G3SIM_AddVtx(&glVerts[i]);
        G3SIM_AddVtx(&glVerts[i + 1]);
    }
}
