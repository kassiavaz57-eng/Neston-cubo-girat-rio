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
// Vertices (x, y, z)
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

// 6 Faces (4 indices per quad)
static const int g_cube_indices[6][4] = {
    { 0, 1, 2, 3 }, // Front
    { 1, 5, 6, 2 }, // Right
    { 5, 4, 7, 6 }, // Back
    { 4, 0, 3, 7 }, // Left
    { 4, 5, 1, 0 }, // Top
    { 3, 2, 6, 7 }  // Bottom
};

// UV Coordinates for 128x128 quad
static const DVECTOR g_uv_coords[4] = {
    {   0,   0 },
    { 127,   0 },
    {   0, 127 },
    { 127, 127 }
};

// Rotation variables
static SVECTOR g_rotation = { 0, 0, 0, 0 };
static VECTOR g_translation = { 0, 0, 480 }; // Distance camera

void init_graphics() {
    ResetGraph(0);

    // Initialize double buffer
    SetDefDispEnv(&g_buffers[0].disp, 0, 0, SCREEN_XRES, SCREEN_YRES);
    SetDefDrawEnv(&g_buffers[0].draw, 0, SCREEN_YRES, SCREEN_XRES, SCREEN_YRES);

    SetDefDispEnv(&g_buffers[1].disp, 0, SCREEN_YRES, SCREEN_XRES, SCREEN_YRES);
    SetDefDrawEnv(&g_buffers[1].draw, 0, 0, SCREEN_XRES, SCREEN_YRES);

    g_buffers[0].draw.isbg = 1;
    setRGB0(&g_buffers[0].draw, 0, 0, 0); // Black background

    g_buffers[1].draw.isbg = 1;
    setRGB0(&g_buffers[1].draw, 0, 0, 0); // Black background

    PutDispEnv(&g_buffers[0].disp);
    PutDrawEnv(&g_buffers[0].draw);

    // Initialize GTE
    InitGeom();
    gte_SetGeomOffset(SCREEN_XRES / 2, SCREEN_YRES / 2); // Center screen
    gte_SetGeomScreen(256); // Perspective focal distance
}

void upload_texture() {
    RECT rect_tex;
    // 4bpp width in 16-bit VRAM words = 128 / 4 = 32
    rect_tex.x = TEX_VRAM_X;
    rect_tex.y = TEX_VRAM_Y;
    rect_tex.w = NESTON_TEX_W / 4; 
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

    // Upload ADPCM sample to SPU RAM
    SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
    SpuWrite((const uint32_t *)audio_adpcm_data, AUDIO_ADPCM_SIZE);
    SpuIsTransferCompleted(SPU_TRANSFER_WAIT);

    // Configure Voice 0
    SpuSetVoiceVolume(0, 0x3FFF, 0x3FFF); // Max volume
    SpuSetVoicePitch(0, 0x1000);          // Normal playback speed (1.0)
    SpuSetVoiceStartAddr(0, SPU_SPRS_ADDR);
    
    // Key ON to play audio
    SpuSetKey(1, 1 << 0);
}

int main() {
    init_graphics();
    upload_texture();
    init_sound();

    // Compute Texture Page (tpage) and CLUT attributes
    uint16_t tpage = getTPage(0, 0, TEX_VRAM_X, TEX_VRAM_Y); // 0 = 4bpp
    uint16_t clut = getClut(CLUT_VRAM_X, CLUT_VRAM_Y);

    SetDispMask(1); // Enable display

    while (1) {
        g_active_buffer ^= 1;
        RenderBuffer* rb = &g_buffers[g_active_buffer];
        uint8_t* prim_ptr = g_prim_buffer[g_active_buffer];
        g_prim_buffer_offset = 0;

        ClearOTagR(rb->ot, OT_LENGTH);

        // Update Cube Rotation
        g_rotation.vx += 12;
        g_rotation.vy += 16;
        g_rotation.vz += 8;

        // GTE Transformation Matrix setup
        MATRIX transform;
        RotMatrix(&g_rotation, &transform);
        TransMatrix(&transform, &g_translation);
        gte_SetRotMatrix(&transform);
        gte_SetTransMatrix(&transform);

        // Render 6 faces
        for (int i = 0; i < 6; i++) {
            POLY_FT4* poly = (POLY_FT4*)(prim_ptr + g_prim_buffer_offset);
            g_prim_buffer_offset += sizeof(POLY_FT4);

            setPolyFT4(poly);
            setRGB0(poly, 128, 128, 128); // Neutral lighting

            // Set UV texture coordinates
            setUV4(poly,
                g_uv_coords[0].vx, g_uv_coords[0].vy,
                g_uv_coords[1].vx, g_uv_coords[1].vy,
                g_uv_coords[2].vx, g_uv_coords[2].vy,
                g_uv_coords[3].vx, g_uv_coords[3].vy
            );

            poly->tpage = tpage;
            poly->clut = clut;

            // Transform vertices with GTE (Tirado o gte_ daqui!)
            long p, flag;
            long otz;

            RotTransPers4(
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

            // Add to Ordering Table if visible and within depth bounds
            if (otz > 0 && otz < OT_LENGTH) {
                addPrim(&rb->ot[otz], poly);
            }
        }

        // Wait for VBlank and swap buffers
        VSync(0);
        PutDispEnv(&rb->disp);
        PutDrawEnv(&rb->draw);
        DrawOTag((uint32_t*)&rb->ot[OT_LENGTH - 1]);
    }

    return 0;
}
