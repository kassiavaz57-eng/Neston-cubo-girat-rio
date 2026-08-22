#include <stdio.h>
#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include <psxetc.h>
#include <psxspu.h>

#include "neston_tex.h"
#include "audio_adpcm.h"

#define SCREEN_XRES 320
#define SCREEN_YRES 240
#define OT_LENGTH 1024

// Double buffering structure
typedef struct {
    DISPENV disp;
    DRAWENV draw;
    uint32_t ot[OT_LENGTH];
} RenderBuffer;

RenderBuffer g_buffers[2];
int g_active_buffer = 0;

// Dynamic Ordering Table & Primitive Buffer
#define PRIM_BUFFER_SIZE 4096
uint8_t g_prim_buffer[2][PRIM_BUFFER_SIZE];
int g_prim_buffer_offset = 0;

// VRAM Allocation positions for Texture and CLUT
#define TEX_VRAM_X 640
#define TEX_VRAM_Y 0
#define CLUT_VRAM_X 0
#define CLUT_VRAM_Y 480

// SPU Allocation Address for Sound
#define SPU_SPRS_ADDR 0x10100

// Cube Geometry Definition
static SVECTOR g_cube_vertices[8] = {
    { -100, -100, -100, 0 },
    {  100, -100, -100, 0 },
    {  100,  100, -100, 0 },
    { -100,  100, -100, 0 },
    { -100, -100,  100, 0 },
    {  100, -100,  100, 0 },
    {  100,  100,  100, 0 },
    { -100,  100,  100, 0 }
};

static const int g_cube_indices[6][4] = {
    { 0, 1, 2, 3 }, // Front
    { 1, 5, 6, 2 }, // Right
    { 5, 4, 7, 6 }, // Back
    { 4, 0, 3, 7 }, // Left
    { 4, 5, 1, 0 }, // Top
    { 3, 2, 6, 7 }  // Bottom
};

static const DVECTOR g_uv_coords[4] = {
    {   0,   0 },
    { 127,   0 },
    {   0, 127 },
    { 127, 127 }
};

static SVECTOR g_rotation = { 0, 0, 0, 0 };
static VECTOR g_translation = { 0, 0, 480 };

// --- A MÁGICA DO META TÁ AQUI ---
static inline long my_RotTransPers4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3,
                                    long *sxy0, long *sxy1, long *sxy2, long *sxy3,
                                    long *otz, long *flag) {
    long _nclip;
    
    // Projeta os 3 primeiros vértices
    gte_ldv0(v0); gte_ldv1(v1); gte_ldv2(v2); gte_rtpt();
    gte_stsxy0(sxy0); gte_stsxy1(sxy1); gte_stsxy2(sxy2);
    
    // Projeta o quarto vértice
    gte_ldv0(v3); gte_rtps(); gte_stsxy(sxy3);
    
    // Pega a profundidade (Z)
    gte_avsz4(); gte_stotz(otz);
    
    // Checa se a face tá virada pra câmera (Culling)
    gte_ldv0(v0); gte_ldv1(v1); gte_ldv2(v2); gte_nclip(); gte_stopz(&_nclip);
    
    gte_stflg(flag); // <-- AQUI TAVA O ERRO! Tirei o "a" do stflag.
    return _nclip;
}
// --------------------------------

void init_graphics() {
    ResetGraph(0);
    SetDefDispEnv(&g_buffers[0].disp, 0, 0, SCREEN_XRES, SCREEN_YRES);
    SetDefDrawEnv(&g_buffers[0].draw, 0, SCREEN_YRES, SCREEN_XRES, SCREEN_YRES);
    SetDefDispEnv(&g_buffers[1].disp, 0, SCREEN_YRES, SCREEN_XRES, SCREEN_YRES);
    SetDefDrawEnv(&g_buffers[1].draw, 0, 0, SCREEN_XRES, SCREEN_YRES);
    g_buffers[0].draw.isbg = 1;
    setRGB0(&g_buffers[0].draw, 0, 0, 0);
    g_buffers[1].draw.isbg = 1;
    setRGB0(&g_buffers[1].draw, 0, 0, 0);
    PutDispEnv(&g_buffers[0].disp);
    PutDrawEnv(&g_buffers[0].draw);
    InitGeom();
    gte_SetGeomOffset(SCREEN_XRES / 2, SCREEN_YRES / 2);
    gte_SetGeomScreen(256);
}

void upload_texture() {
    RECT rect_tex;
    rect_tex.x = TEX_VRAM_X;
    rect_tex.y = TEX_VRAM_Y;
    // O aviso do TEX_H é porque no arquivo neston_tex.h o nome tá duplicado, mas não dá erro.
    rect_tex.w = NESTON_TEX_H / 4; 
    rect_tex.h = NESTON_TEX_H;
    LoadImage(&rect_tex, (uint32_t*)neston_tex_bytes);
    DrawSync(0);

    RECT rect_clut;
    rect_clut.x = CLUT_VRAM_X;
    rect_clut.y = CLUT_VRAM_Y;
    rect_clut.w = 16;
    rect_clut.h = 1;
    LoadImage(&rect_clut, (uint32_t*)neston_clut);
    DrawSync(0);
}

void init_sound() {
    SpuInit();
    SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
    SpuWrite((const uint32_t *)audio_adpcm_data, AUDIO_ADPCM_SIZE);
    SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
    SpuSetVoiceVolume(0, 0x3FFF, 0x3FFF);
    SpuSetVoicePitch(0, 0x1000);
    SpuSetVoiceStartAddr(0, SPU_SPRS_ADDR);
    SpuSetKey(1, 1 << 0);
}

int main() {
    init_graphics();
    upload_texture();
    init_sound();

    uint16_t tpage = getTPage(0, 0, TEX_VRAM_X, TEX_VRAM_Y);
    uint16_t clut = getClut(CLUT_VRAM_X, CLUT_VRAM_Y);

    SetDispMask(1);

    while (1) {
        g_active_buffer ^= 1;
        RenderBuffer* rb = &g_buffers[g_active_buffer];
        uint8_t* prim_ptr = g_prim_buffer[g_active_buffer];
        g_prim_buffer_offset = 0;

        ClearOTagR(rb->ot, OT_LENGTH);

        g_rotation.vx += 12;
        g_rotation.vy += 16;
        g_rotation.vz += 8;

        MATRIX transform;
        RotMatrix(&g_rotation, &transform);
        TransMatrix(&transform, &g_translation);
        gte_SetRotMatrix(&transform);
        gte_SetTransMatrix(&transform);

        for (int i = 0; i < 6; i++) {
            POLY_FT4* poly = (POLY_FT4*)(prim_ptr + g_prim_buffer_offset);
            
            setPolyFT4(poly);
            setRGB0(poly, 128, 128, 128);
            setUV4(poly,
                g_uv_coords[0].vx, g_uv_coords[0].vy,
                g_uv_coords[1].vx, g_uv_coords[1].vy,
                g_uv_coords[2].vx, g_uv_coords[2].vy,
                g_uv_coords[3].vx, g_uv_coords[3].vy
            );
            poly->tpage = tpage;
            poly->clut = clut;

            long flag;
            long otz;
            
            // Usando a função nova aqui!
            long nclip = my_RotTransPers4(
                &g_cube_vertices[g_cube_indices[i][0]],
                &g_cube_vertices[g_cube_indices[i][1]],
                &g_cube_vertices[g_cube_indices[i][2]],
                &g_cube_vertices[g_cube_indices[i][3]],
                (long*)&poly->x0,
                (long*)&poly->x1,
                (long*)&poly->x2,
                (long*)&poly->x3,
                &otz, &flag
            );

            // Só desenha se tiver virado pra frente (nclip > 0) e no limite da tela
            if (nclip > 0 && otz > 0 && otz < OT_LENGTH) {
                g_prim_buffer_offset += sizeof(POLY_FT4);
                addPrim(&rb->ot[otz], poly);
            }
        }

        VSync(0);
        PutDispEnv(&rb->disp);
        PutDrawEnv(&rb->draw);
        DrawOTag((uint32_t*)&rb->ot[OT_LENGTH - 1]);
    }

    return 0;
}
