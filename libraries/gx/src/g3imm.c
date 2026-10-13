#include <nitro/gx/g3imm.h>
#include <nitro/mi/dma.h>
#include "../include/gxasm.h"

void G3_LoadMtx44 (const MtxFx44 *m)
{
	SDK_NULL_ASSERT(m);
    #ifdef SDK_PORT
    draw_msg_t * msg;
    msg = malloc( sizeof( draw_msg_t ) );

    msg->type = DRAW_CMD_G3_LOADMTX44;
    memcpy( &msg->data.mtx44, m, sizeof( MtxFx44 ) );

    SIM_HandleG3Command( msg );
    free( msg );
    #else

	reg_G3X_GXFIFO = G3OP_MTX_LOAD_4x4;
	GX_SendFifo64B(&m->_00, (void *)&reg_G3X_GXFIFO);
    #endif
}

void G3_LoadMtx43 (const MtxFx43 *m)
{
	SDK_NULL_ASSERT(m);
    #ifdef SDK_PORT
    draw_msg_t * msg;
    msg = malloc( sizeof( draw_msg_t ) );

    msg->type = DRAW_CMD_G3_LOADMTX43;
    memcpy( &msg->data.mtx43, m, sizeof( MtxFx43 ) );

    SIM_HandleG3Command( msg );
    free( msg );
    #else

	reg_G3X_GXFIFO = G3OP_MTX_LOAD_4x3;
	GX_SendFifo48B(&m->_00, (void *)&reg_G3X_GXFIFO);
    #endif
}

void G3_MultMtx44 (const MtxFx44 *m)
{
	SDK_NULL_ASSERT(m);
    #ifdef SDK_PORT
    draw_msg_t * msg;
    msg = malloc( sizeof( draw_msg_t ) );

    msg->type = DRAW_CMD_G3_MTXMULT44;
    memcpy( &msg->data.mtx44, m, sizeof( MtxFx44 ) );

    SIM_HandleG3Command( msg );
    free( msg );
    #else

	reg_G3X_GXFIFO = G3OP_MTX_MULT_4x4;
	GX_SendFifo64B(&m->_00, (void *)&reg_G3X_GXFIFO);
    #endif
}

void G3_MultMtx43 (const MtxFx43 *m)
{
	SDK_NULL_ASSERT(m);
    #ifdef SDK_PORT
    draw_msg_t * msg;
    msg = malloc( sizeof( draw_msg_t ) );

    msg->type = DRAW_CMD_G3_MTXMULT43;
    memcpy( &msg->data.mtx43, m, sizeof( MtxFx43 ) );

    SIM_HandleG3Command( msg );
    free( msg );
    #else

	reg_G3X_GXFIFO = G3OP_MTX_MULT_4x3;
	GX_SendFifo48B(&m->_00, (void *)&reg_G3X_GXFIFO);
    #endif
}

void G3_MultMtx33 (const MtxFx33 *m)
{
	SDK_NULL_ASSERT(m);
    #ifdef SDK_PORT
    // The FIFO is one register here, so the 36 bytes would overrun it into GXSTAT and the registers after it
    draw_msg_t * msg;
    msg = malloc( sizeof( draw_msg_t ) );

    msg->type = DRAW_CMD_G3_MTXMULT33;
    memcpy( &msg->data.mtx33, m, sizeof( MtxFx33 ) );

    SIM_HandleG3Command( msg );
    free( msg );
    #else

	reg_G3X_GXFIFO = G3OP_MTX_MULT_3x3;
	GX_SendFifo36B(&m->_00, (void *)&reg_G3X_GXFIFO);
    #endif
}

void G3_Shininess (const u32 *table)
{
	SDK_NULL_ASSERT(table);

#ifdef SDK_PORT
    draw_msg_t msg = {0};
    msg.type = DRAW_CMD_G3_SHININESS;
    memcpy(msg.data.numsU32, table, 128);
    SIM_HandleG3Command(&msg);
#else
	reg_G3X_GXFIFO = G3OP_SHININESS;
	GX_SendFifo128B(&table[0], (void *)&reg_G3X_GXFIFO);
#endif
}

void G3_MultTransMtx33 (const MtxFx33 *mtx, const VecFx32 *trans)
{
	SDK_NULL_ASSERT(mtx);
	SDK_NULL_ASSERT(trans);

    #ifdef SDK_PORT
    draw_msg_t * msg;
    msg = malloc( sizeof( draw_msg_t ) );

    msg->type = DRAW_CMD_G3_MTXMULT43;
    msg->data.mtx43._00 = mtx->_00;
    msg->data.mtx43._01 = mtx->_01;
    msg->data.mtx43._02 = mtx->_02;
    msg->data.mtx43._10 = mtx->_10;
    msg->data.mtx43._11 = mtx->_11;
    msg->data.mtx43._12 = mtx->_12;
    msg->data.mtx43._20 = mtx->_20;
    msg->data.mtx43._21 = mtx->_21;
    msg->data.mtx43._22 = mtx->_22;
    msg->data.mtx43._30 = trans->x;
    msg->data.mtx43._31 = trans->y;
    msg->data.mtx43._32 = trans->z;

    SIM_HandleG3Command( msg );
    free( msg );
    #else
	reg_G3X_GXFIFO = G3OP_MTX_MULT_4x3;
	GX_SendFifo36B(&mtx->_00, (void *)&reg_G3X_GXFIFO);

	reg_G3X_GXFIFO = (u32)trans->x;
	reg_G3X_GXFIFO = (u32)trans->y;
	reg_G3X_GXFIFO = (u32)trans->z;
    #endif
}
