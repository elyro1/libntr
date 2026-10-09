#if defined(SDK_BUILD_LINUX) || defined(SDK_BUILD_WIN64)
#include <simulator/sim.h>
#endif
#include <simulator/g3_draw.h>

#include <simulator/g3_handler.h>
#include <simulator/assert.h>
#include <string.h>
#include <stddef.h>

#include <nitro.h>
#include <nitro/gx/g3.h>
#include <nitro/gx/gx_load.h>
#include <nitro/gx/gx_vramcnt.h>
#include <unordered_map>
#include <simulator/sim_crc32.h>

#ifdef SDK_BUILD_LINUX
#include <signal.h>
#endif

#ifdef SDK_TRACY_ENABLE
#include "tracy/TracyC.h"
#endif

#define G3_DRAW_MAX_ITEMS 1000

#define getbit(x,n) ( ( (x) >> (n) ) & 1 )

// Deferred polygons must retain their own fog state. Flashlight models disable
// fog, so inheriting the last submitted polygon's uniforms lights the whole gym.
typedef struct {
	int enabled;
	u32 control;
	u32 color;
	u32 offset;
	u8 density[32];
} g3_fog_state_t;

typedef struct {
	float zIdx;
	GLuint textureId;
	G3SIM_PolygonAttr_t polygonAttr;
	g3_fog_state_t fog;
	G3SIM_TexImageParam_t texImageParam;
	G3SIM_Vertex_t * verts;
	u32 vertsCount;
} g3_draw_item_t;

G3SIM_Vertex_t s_G3DrawVerts[G3_DRAW_MAX_VERTS];

// Texture cache based on the CRC of the texture and palette
std::unordered_map<u64, u8*> sTextureCache;

//The item list is used for drawing translucent things sorted by Z position
static g3_draw_item_t s_G3DrawItemList[G3_DRAW_MAX_ITEMS];
static u32 s_G3DrawItemListCount = 0;

static u32 s_G3DrawCurVertIdx = 0;
static GLuint s_G3DrawVertexArray;
static GLuint s_G3DrawVertexBuffer;

extern G3SIM_TexImageParam_t s_texImageParam;
extern G3SIM_PolygonAttr_t s_curPolygonAttr;
extern GLuint g3shaderProgramID;
extern u8 s_SIM_g3tex[4*1024*1024];
extern GXVRamTex s_SIM_GXVRamTex;
extern GXVRamTexPltt s_SIM_GXVRamTexPltt;


// The CPU cache supplies decoded pixels. Allocate upload targets only for
// dimensions actually used, instead of preallocating the old fixed-size pool.
// Deferred draws still own independent copies of their decoded textures.
static GLuint SIM_GetTextureID() {
    static std::unordered_map<u32, GLuint> targets;
    const u32 width = s_texImageParam.textureSSize;
    const u32 height = s_texImageParam.textureTSize;
    const u32 key = (width << 16) | height;
    GLuint &id = targets[key];
    glActiveTexture(GL_TEXTURE0);
    if (!id) {
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindTexture(GL_TEXTURE_2D, id);
    return id;
}

static void* getTextureVramBank();
static void* getTexPlttVramBank();

static void* getTextureVramBank()
{
	void * vramBank = NULL;
	switch( s_SIM_GXVRamTex ){
		case GX_VRAM_TEX_0_A:
		case GX_VRAM_TEX_01_AB:
		case GX_VRAM_TEX_012_ABC:
		case GX_VRAM_TEX_0123_ABCD:
		case GX_VRAM_TEX_01_AC:
		case GX_VRAM_TEX_01_AD:
		case GX_VRAM_TEX_012_ABD:
		case GX_VRAM_TEX_012_ACD:
			vramBank = (void *)HW_LCDC_VRAM_A;
			break;
		case GX_VRAM_TEX_0_B:
		case GX_VRAM_TEX_01_BC:
		case GX_VRAM_TEX_012_BCD:
		case GX_VRAM_TEX_01_BD:
			vramBank = (void *)HW_LCDC_VRAM_B;
			break;
		case GX_VRAM_TEX_0_C:
		case GX_VRAM_TEX_01_CD:
			vramBank = (void *)HW_LCDC_VRAM_C;
			break;
		case GX_VRAM_TEX_0_D:
			vramBank = (void *)HW_LCDC_VRAM_A;
			break;
		default:
			//?
			break;
	}
	
	return vramBank;
}

static void* getTexPlttVramBank()
{
	void * vramBank = NULL;
	switch( s_SIM_GXVRamTexPltt ) {
		case GX_VRAM_TEXPLTT_NONE:
		case GX_VRAM_TEXPLTT_0123_E:
		case GX_VRAM_TEXPLTT_01234_EF:
		case GX_VRAM_TEXPLTT_012345_EFG:
			vramBank = (void *)HW_LCDC_VRAM_E;
			break;
		case GX_VRAM_TEXPLTT_0_F:
		case GX_VRAM_TEXPLTT_01_FG:
			vramBank = (void *)HW_LCDC_VRAM_F;
			break;
		case GX_VRAM_TEXPLTT_0_G:
			vramBank = (void *)HW_LCDC_VRAM_G;
			break;
	}
	
	return vramBank;
}

void G3SIM_AddVtx(G3SIM_Vertex_t * vtx)
{
	memcpy(&s_G3DrawVerts[s_G3DrawCurVertIdx], vtx, sizeof(G3SIM_Vertex_t));

	s_G3DrawCurVertIdx = s_G3DrawCurVertIdx + 1;

	SIM_assert(s_G3DrawCurVertIdx < G3_DRAW_MAX_VERTS);
	return;
}

void G3SIM_DrawArray()
{
	if( s_G3DrawCurVertIdx != 0 )
	{
		glBindVertexArray(s_G3DrawVertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, s_G3DrawVertexBuffer);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(G3SIM_Vertex_t) * s_G3DrawCurVertIdx, s_G3DrawVerts);
		glDrawArrays(GL_TRIANGLES,0,s_G3DrawCurVertIdx);
	}
	return;
}

void G3SIM_DrawCleanUp()
{
	s_G3DrawCurVertIdx = 0;
}

void G3SIM_DrawInit()
{
	glGenVertexArrays(1, &s_G3DrawVertexArray);
	glBindVertexArray(s_G3DrawVertexArray);

	glGenBuffers(1, &s_G3DrawVertexBuffer);

	glBindBuffer(GL_ARRAY_BUFFER, s_G3DrawVertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(s_G3DrawVerts), NULL, GL_DYNAMIC_DRAW);


	glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t), (void*)offsetof(G3SIM_Vertex_t, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t), (void*)offsetof(G3SIM_Vertex_t, s));
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t), (void*)offsetof(G3SIM_Vertex_t, r));

	glBindVertexArray(0);
	glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
}

static void G3SIM_ApplyFog(const g3_fog_state_t &fog)
{
	// Set the fog attributes in the shader
	GLint useFogLoc = glGetUniformLocation(g3shaderProgramID, "useFog");
	int useFog = fog.enabled;
	glProgramUniform1i(g3shaderProgramID, useFogLoc , useFog);
	if(useFog) {
		// Fog color
		GLint fogColorLoc = glGetUniformLocation(g3shaderProgramID, "fogColor");
		u32 fogRed = fog.color & 0b11111;
		u32 fogGreen = (fog.color >> 5) & 0b11111;
		u32 fogBlue = (fog.color >> 10) & 0b11111;
		u32 fogAlpha = (fog.color >> 16) & 0b11111;
		float fogRedF = (float)fogRed / 32.0f;
		float fogGreenF = (float)fogGreen / 32.0f;
		float fogBlueF = (float)fogBlue / 32.0f;
		float fogAlphaF = (float)fogAlpha / 32.0f;
		glProgramUniform4f(g3shaderProgramID, fogColorLoc, fogRedF, fogGreenF, fogBlueF, fogAlphaF);


		u32 fogShift = (fog.control >> 8) & 0b1111;
		u32 fogStep = 0x400 >> fogShift;

		//Fog depth boundaries
		u32 fogOffset = fog.offset;

		float fogDepthBoundary[32];
		float fogDensity[32];
		const u8 * fogTablePtr = fog.density;
		for(int i=0; i < 32; i++) {
			fogDepthBoundary[i] = ((float)(fogOffset + fogStep*(i+1)) / 32767.0f) * 2.0f - 1.0f;
			fogDensity[i] = (float)fogTablePtr[i] / 127.0f;
		}

		GLint fogDepthBoundaryLoc = glGetUniformLocation(g3shaderProgramID, "fogDepthBoundary");
		glProgramUniform1fv(g3shaderProgramID, fogDepthBoundaryLoc, 32, fogDepthBoundary);

		GLint fogDensityLoc = glGetUniformLocation(g3shaderProgramID, "fogDensity");
		glProgramUniform1fv(g3shaderProgramID, fogDensityLoc, 32, fogDensity);
	}

}

static void G3SIM_ApplyTextureWrap(const G3SIM_TexImageParam_t &texture)
{
	// Flip applies only to repeated coordinates; otherwise the DS clamps to
	// the edge texel. Repeating clamped effect textures exposes extra copies.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
		!texture.repeatS ? GL_CLAMP_TO_EDGE : (texture.flipS ? GL_MIRRORED_REPEAT : GL_REPEAT));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
		!texture.repeatT ? GL_CLAMP_TO_EDGE : (texture.flipT ? GL_MIRRORED_REPEAT : GL_REPEAT));
}

static void G3SIM_ApplyPolygonRasterState(const G3SIM_PolygonAttr_t &polygon)
{
	glDepthFunc(polygon.depthTest ? GL_LEQUAL : GL_LESS);
	if(polygon.cullMode == GX_CULL_NONE) {
		glDisable(GL_CULL_FACE);
	} else {
		glEnable(GL_CULL_FACE);
		glCullFace(polygon.cullMode == GX_CULL_ALL ? GL_FRONT_AND_BACK :
			(polygon.cullMode == GX_CULL_FRONT ? GL_BACK : GL_FRONT));
	}
}

void G3SIM_FlushArray()
{
	if(s_G3DrawCurVertIdx == 0) {
		// No verts to draw
		return;
	}

	#ifdef SDK_TRACY_ENABLE
	TracyCZone(FlushArrayZone, 1);
	#endif

	u8 * texBuf = nullptr;

	if( s_texImageParam.textureFormat != GX_TEXFMT_NONE )
	{
		GLuint g3TextureId;
		g3TextureId = SIM_GetTextureID();
		
		glActiveTexture(GL_TEXTURE0);
		glBindTexture( GL_TEXTURE_2D, g3TextureId );

		G3SIM_ApplyTextureWrap(s_texImageParam);

        u8 *vramTex;
#if defined(SDK_BUILD_LINUX) || defined(SDK_BUILD_WIN64)
        if (s_texImageParam.textureOffset >= 0x80000)
            vramTex = (u8 *)SIM_GetExtendedTextureStorage(s_texImageParam.textureOffset, 0);
        else
#endif
            vramTex = (u8 *)(getTextureVramBank() + s_texImageParam.textureOffset);
		u8 * vramPltt = (u8*)(getTexPlttVramBank() + s_texPlttBase);
		u16 * colorAddr = (u16*)vramPltt;

		//Calculate the texture + palette CRC
		u32 paletteCRC = SIM_crc32buf(vramPltt, 512);
		u32 vramTexBufSize = 0;

		if(s_texImageParam.textureFormat == GX_TEXFMT_PLTT4) {
			vramTexBufSize = (s_texImageParam.textureSSize * s_texImageParam.textureTSize) >> 2;
		} else if(s_texImageParam.textureFormat == GX_TEXFMT_PLTT16) {
			vramTexBufSize = (s_texImageParam.textureSSize * s_texImageParam.textureTSize) >> 1;
		} else {
			vramTexBufSize = (s_texImageParam.textureSSize * s_texImageParam.textureTSize);
		}

		u32 textureCRC = SIM_crc32buf(vramTex, vramTexBufSize);

		u64 finalCRC = (paletteCRC | ((u64)textureCRC << 32));
		

		if(sTextureCache.count(finalCRC)) {
			// Texture is in the cache
			texBuf = sTextureCache[finalCRC];
		} else {
			//Convert the DS texture data into a format opengl can understand
			u8 * outTexBuf = new u8[4*s_texImageParam.textureSSize * s_texImageParam.textureTSize];
			memset((void*)outTexBuf, 0, 4*s_texImageParam.textureSSize * s_texImageParam.textureTSize);


			switch( s_texImageParam.textureFormat ){
				case GX_TEXFMT_A3I5:
					G3SIM_DecodeTexA3I5(vramTex, colorAddr, outTexBuf, s_texImageParam.textureSSize, s_texImageParam.textureTSize);
					break;
				case GX_TEXFMT_PLTT4:
					G3SIM_DecodeTex4(vramTex, colorAddr, outTexBuf, s_texImageParam.textureSSize, s_texImageParam.textureTSize);
					break;
				case GX_TEXFMT_PLTT16:
					G3SIM_DecodeTex16(vramTex, colorAddr, outTexBuf, s_texImageParam.textureSSize, s_texImageParam.textureTSize);
					break;
				case GX_TEXFMT_PLTT256:
					G3SIM_DecodeTex256(vramTex, colorAddr, outTexBuf, s_texImageParam.textureSSize, s_texImageParam.textureTSize);
					break;
				case GX_TEXFMT_COMP4x4:
					SIM_assert_always();
					//TODO
					break;
				case GX_TEXFMT_A5I3:
					G3SIM_DecodeTexA5I3(vramTex, colorAddr, outTexBuf, s_texImageParam.textureSSize, s_texImageParam.textureTSize);
					break;
				case GX_TEXFMT_DIRECT:
					G3SIM_DecodeTexDirect(vramTex, outTexBuf, s_texImageParam.textureSSize, s_texImageParam.textureTSize);
					break;
			}

			sTextureCache[finalCRC] = outTexBuf;
			texBuf = outTexBuf;
		}

		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s_texImageParam.textureSSize, s_texImageParam.textureTSize, GL_RGBA, GL_UNSIGNED_BYTE, (void *)texBuf);

		GLint texUnitLoc = glGetUniformLocation(g3shaderProgramID, "myTexture");
		//set texture 0 in the shader
		glProgramUniform1i(g3shaderProgramID, texUnitLoc , 0);

		//Enable texture in the shader
		glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "useTexture") , 1);
	}
	else
	{
		//Disable texture in the shader
		glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "useTexture") , 0);							
	}

	// Set the polygon mode in the shader
	GLint polygonModeLoc = glGetUniformLocation(g3shaderProgramID, "polygonMode");
	glProgramUniform1i(g3shaderProgramID, polygonModeLoc , s_curPolygonAttr.polygonMode);

	g3_fog_state_t fog = {};
	fog.control = s_reg_G3X_DISP3DCNT;
	fog.enabled = getbit(fog.control, 7) && s_curPolygonAttr.fogEnable;
	fog.color = s_reg_G3X_FOG_COLOR;
	fog.offset = s_reg_G3X_FOG_OFFSET;
	memcpy(fog.density, (const void *)&s_reg_G3X_FOG_TABLE_0, sizeof(fog.density));
	G3SIM_ApplyFog(fog);


	// If this is a translucent draw, save it for the end of the frame to render it last
	if(
		( s_texImageParam.textureFormat == GX_TEXFMT_A3I5 
	   || s_texImageParam.textureFormat == GX_TEXFMT_A5I3
	   || s_curPolygonAttr.alpha < 0.99f )
	 && s_G3DrawCurVertIdx != 0) {
		// A3I5/A5I3 textures can contain opaque texels. Draw those with the
		// opaque scene; defer only their translucent texels for blending.
		if(s_curPolygonAttr.alphaInt == 31) {
			glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "alphaPass"), 1);
			glDepthMask(GL_TRUE);
			G3SIM_DrawArray();
		}
		glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "alphaPass"), 0);
		g3_draw_item_t * item = &s_G3DrawItemList[s_G3DrawItemListCount];

		memset(item, 0, sizeof(g3_draw_item_t));


		if(s_texImageParam.textureFormat != GX_TEXFMT_NONE) {
			//Allocate and store off the texture
			glGenTextures(1, &item->textureId);
			glBindTexture(GL_TEXTURE_2D, item->textureId);
        	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			G3SIM_ApplyTextureWrap(s_texImageParam);
	
        	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_texImageParam.textureSSize, s_texImageParam.textureTSize, 0,
        	         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s_texImageParam.textureSSize, s_texImageParam.textureTSize, GL_RGBA, GL_UNSIGNED_BYTE, (void *)texBuf);
		}

		item->fog = fog;

		//Copy over the polygonattr
		memcpy(&item->polygonAttr, &s_curPolygonAttr, sizeof(G3SIM_PolygonAttr_t));

		//Copy over the texImageParam
		memcpy(&item->texImageParam, &s_texImageParam, sizeof(G3SIM_TexImageParam_t));

		//Allocate and copy the verts
		item->verts = (G3SIM_Vertex_t*)(malloc(sizeof(G3SIM_Vertex_t) * s_G3DrawCurVertIdx));
		item->vertsCount = s_G3DrawCurVertIdx;
		memcpy(item->verts, s_G3DrawVerts, sizeof(G3SIM_Vertex_t) * s_G3DrawCurVertIdx);

		s_G3DrawItemListCount++;
	} else {
		glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "alphaPass"), 0);
		glDepthMask(GL_TRUE);
		G3SIM_DrawArray();
	}

	G3SIM_DrawCleanUp();
	#ifdef SDK_TRACY_ENABLE
	TracyCZoneEnd(FlushArrayZone);
	#endif
}

void G3SIM_DrawItems()
{
	//TODO sort the items by Z coordinate


	// The DS blends a translucent pixel only when its polygon ID differs from
	// the last translucent pixel at that location. Bit 6 distinguishes ID zero
	// from a pixel that has not been blended yet. Keep this across queue flushes;
	// the stencil buffer is reset with the 3D framebuffer each frame.
	glEnable(GL_STENCIL_TEST);
	glStencilMask(0x7f);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

	//Draws all the translucent objects that were deferred to the end of the frame.
	for(int i=0; i<s_G3DrawItemListCount; i++) {
		g3_draw_item_t * item = &s_G3DrawItemList[i];
		glStencilFunc(GL_NOTEQUAL, 0x40 | item->polygonAttr.polygonId, 0x7f);
		if(item->textureId != 0){
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, item->textureId);
			//Enable texture in the shader
			glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "useTexture") , 1);
		} else {
			glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "useTexture") , 0);
		}

		if(item->textureId != 0) {
			G3SIM_ApplyTextureWrap(item->texImageParam);
		}

		// Set the polygon mode in the shader
		GLint polygonModeLoc = glGetUniformLocation(g3shaderProgramID, "polygonMode");
		glProgramUniform1i(g3shaderProgramID, polygonModeLoc, item->polygonAttr.polygonMode);

		G3SIM_ApplyFog(item->fog);
		G3SIM_ApplyPolygonRasterState(item->polygonAttr);
		glDepthMask(item->polygonAttr.translucentDepth ? GL_TRUE : GL_FALSE);
		glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "alphaPass"), 2);

		glBindVertexArray(s_G3DrawVertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, s_G3DrawVertexBuffer);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(G3SIM_Vertex_t) * item->vertsCount, item->verts);
		glDrawArrays(GL_TRIANGLES,0, item->vertsCount);

		free(item->verts);

		if(item->textureId != 0) {
			glDeleteTextures(1, &item->textureId);
		}
	}
	s_G3DrawItemListCount = 0;
	glDisable(GL_STENCIL_TEST);
	glStencilMask(0xff);
	glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "alphaPass"), 0);
	glDepthMask(GL_TRUE);
	G3SIM_ApplyPolygonRasterState(s_curPolygonAttr);
}
