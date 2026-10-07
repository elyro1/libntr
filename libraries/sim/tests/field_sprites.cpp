// Run with: python3 libraries/sim/tests/test_field_sprites.py /path/to/build_linux
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
extern volatile uint32_t s_reg_GX_DISPCNT;
extern unsigned char s_HW_OBJ_VRAM[];
extern unsigned char s_HW_OAM[];
void SIM_SetFieldWideSpriteRange(uint32_t,uint32_t,int);
void SIM_SetFieldTiledSpriteRange(uint32_t,uint32_t,int);
void SIM_handleAssertionFailure(const char *,unsigned,const char *) { std::abort(); }
}
extern uint32_t s_FieldSpriteLine[];
uint32_t OBJLine[2][256];
unsigned char OBJWindow[2][256];
unsigned char s_SIM_DBG_OAMenable=1,s_SIM_DBG_OAMSenable=1;
unsigned char s_mosaicTable[16][256];
int s_SIM_GXSubBgExtPltt=0,s_SIM_GXBgExtPltt=0;
void G2SIM_DrawOBJ(uint32_t,uint8_t,int,int);
int G2SIM_HasFieldSprites();
static void check(bool ok,const char *message) { if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);} }
static void sprite(int x,bool affine) {
 auto *oam=reinterpret_cast<uint16_t *>(s_HW_OAM);
 for(int i=0;i<128;i++) {oam[i*4]=0x200;oam[i*4+1]=0;oam[i*4+2]=0;}
 oam[0]=affine?0x300:0;
 oam[1]=0xc000|(x&511);
 oam[2]=32; // 64x64, tile data at byte 1024, native priority zero.
 oam[3]=256; oam[7]=0; oam[11]=0; oam[15]=256;
 std::memset(s_HW_OBJ_VRAM+1024,0x11,2048);
 std::memset(OBJLine,0,sizeof OBJLine);
 std::memset(s_FieldSpriteLine,0,448*4);
}
int main(){
 s_reg_GX_DISPCNT=0x1010;
 SIM_SetFieldWideSpriteRange(1024,2048,1);
 check(G2SIM_HasFieldSprites(),"registration");
 sprite(240,false);
 G2SIM_DrawOBJ(16,0,0,0);
 check((OBJLine[0][255]&255)==1,"native sprite unchanged");
 G2SIM_DrawOBJ(16,0,0,1);
 check((s_FieldSpriteLine[96+255]&255)==1,"native right boundary");
 check((s_FieldSpriteLine[96+256]&255)==1,"continuation past right edge");
 check((s_FieldSpriteLine[96+303]&255)==1,"full right sprite");
 check(s_FieldSpriteLine[96+304]==0,"right sprite extent");
 sprite(288,false);G2SIM_DrawOBJ(16,0,0,1);
 check((s_FieldSpriteLine[96+310]&255)==1,"OAM-wrapped right cloud");
 sprite(-16,false);G2SIM_DrawOBJ(16,0,0,1);
 check((s_FieldSpriteLine[96-16]&255)==1,"continuation left of native edge");
 check((s_FieldSpriteLine[96]&255)==1,"left boundary continuity");
 sprite(240,true);G2SIM_DrawOBJ(48,0,0,1);
 check((s_FieldSpriteLine[96+280]&255)==1,"affine sprite continues past native edge");
 check(s_FieldSpriteLine[96+271]==0,"affine transform preserved");
 // Normal UI tiles are not registered, so they never reach the side buffer.
 SIM_SetFieldWideSpriteRange(1024,0,0);
 sprite(240,false);G2SIM_DrawOBJ(16,0,0,1);
 check(s_FieldSpriteLine[96+260]==0,"unregistered UI excluded");
 // Existing snow-style repeats remain at exactly 256-pixel intervals.
 SIM_SetFieldTiledSpriteRange(1024,2048,1);
 sprite(8,false);G2SIM_DrawOBJ(16,0,0,1);
 check((s_FieldSpriteLine[96+8+256]&255)==1,"weather repeat retained");
 check(s_FieldSpriteLine[96+7+256]==0,"weather repeat position");
 SIM_SetFieldTiledSpriteRange(1024,0,0);
 check(!G2SIM_HasFieldSprites(),"cleanup on scene exit");
 puts("PASS: left/right cloud continuation, affine sprites, native rendering, UI exclusion, snow repetition and cleanup");
}
