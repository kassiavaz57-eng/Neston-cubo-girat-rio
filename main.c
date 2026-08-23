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

#define PRIM_BUFFER_SIZE 4096

/* ============================================================
   VRAM
   ============================================================ */

#define TEX_VRAM_X 640
#define TEX_VRAM_Y 0

#define CLUT_VRAM_X 0
#define CLUT_VRAM_Y 480

/* ============================================================
   SPU
   ============================================================ */

#define SPU_SPRS_ADDR 0x10100

/* ============================================================
   FRAMEBUFFERS
   ============================================================ */

typedef struct {
    DISPENV disp;
    DRAWENV draw;
    uint32_t ot[OT_LENGTH];
} RenderBuffer;

static RenderBuffer g_buffers[2];

static int g_active_buffer = 0;

static uint8_t g_prim_buffer[2][PRIM_BUFFER_SIZE];

static int g_prim_buffer_offset = 0;

/* ============================================================
   CUBO
   ============================================================ */

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

/*
 * IMPORTANTE:
 *
 * O POLY_FT4 (e o POLY_G4) do PS1 espera os 4 vértices
 * no padrão "Z":
 *
 *   v0 --- v1
 *   |       |
 *   v2 --- v3
 *
 * ou seja, v1-v2 tem que ser uma DIAGONAL do quadrado,
 * não uma aresta. A GPU desenha o quad como dois
 * triângulos (v0,v1,v2) e (v1,v2,v3) internamente.
 *
 * Os índices abaixo estavam em ordem de perímetro
 * (andando pela borda do quadrado), o que faz v1-v2
 * ser uma aresta em vez de diagonal — daí o quad sair
 * torto/"borboleta" com a textura cortada na diagonal.
 */
static const int g_cube_indices[6][4] = {
    { 3, 2, 0, 1 },
    { 2, 6, 1, 5 },
    { 6, 7, 5, 4 },
    { 7, 3, 4, 0 },
    { 0, 1, 4, 5 },
    { 7, 6, 3, 2 }
};

/*
 * Textura 128x128.
 */
static const DVECTOR g_uv_coords[4] = {
    {   0,   0 },
    { 127,   0 },
    {   0, 127 },
    { 127, 127 }
};

static SVECTOR g_rotation = {
    0, 0, 0, 0
};

static VECTOR g_translation = {
    0, 0, 480
};

/* ============================================================
   GTE
   ============================================================ */

static inline long my_RotTransPers4(
    SVECTOR *v0,
    SVECTOR *v1,
    SVECTOR *v2,
    SVECTOR *v3,

    long *sxy0,
    long *sxy1,
    long *sxy2,
    long *sxy3,

    long *otz,
    long *flag
) {
    long nclip;

    /*
     * Primeiros três vértices.
     */
    gte_ldv0(v0);
    gte_ldv1(v1);
    gte_ldv2(v2);

    gte_rtpt();

    gte_stsxy0(sxy0);
    gte_stsxy1(sxy1);
    gte_stsxy2(sxy2);

    /*
     * Backface culling.
     *
     * IMPORTANTE: isso precisa rodar AQUI, logo após o
     * rtpt(), enquanto o FIFO de SXY ainda contém
     * v0,v1,v2. Se a gente chamar rtps() do 4º vértice
     * antes, o FIFO desliza (SXY0<-v1, SXY1<-v2, SXY2<-v3)
     * e o nclip passa a testar o triângulo v1,v2,v3 em
     * vez de v0,v1,v2 (era isso que o código antigo fazia,
     * recarregando v0,v1,v2 nos registradores errados —
     * gte_nclip() não lê V0/V1/V2, ele lê o FIFO de SXY).
     */
    gte_nclip();

    gte_stopz(&nclip);

    gte_stflg(flag);

    /*
     * Quarto vértice.
     */
    gte_ldv0(v3);

    gte_rtps();

    gte_stsxy(sxy3);

    /*
     * Profundidade.
     */
    gte_avsz4();

    gte_stotz(otz);

    return nclip;
}

/* ============================================================
   GRÁFICOS
   ============================================================ */

static void init_graphics(void) {

    ResetGraph(0);

    /*
     * Buffer 0.
     */
    SetDefDispEnv(
        &g_buffers[0].disp,
        0,
        0,
        SCREEN_XRES,
        SCREEN_YRES
    );

    SetDefDrawEnv(
        &g_buffers[0].draw,
        0,
        SCREEN_YRES,
        SCREEN_XRES,
        SCREEN_YRES
    );

    /*
     * Buffer 1.
     */
    SetDefDispEnv(
        &g_buffers[1].disp,
        0,
        SCREEN_YRES,
        SCREEN_XRES,
        SCREEN_YRES
    );

    SetDefDrawEnv(
        &g_buffers[1].draw,
        0,
        0,
        SCREEN_XRES,
        SCREEN_YRES
    );

    /*
     * Fundo preto.
     */
    g_buffers[0].draw.isbg = 1;
    setRGB0(
        &g_buffers[0].draw,
        0,
        0,
        0
    );

    g_buffers[1].draw.isbg = 1;
    setRGB0(
        &g_buffers[1].draw,
        0,
        0,
        0
    );

    PutDispEnv(
        &g_buffers[0].disp
    );

    PutDrawEnv(
        &g_buffers[0].draw
    );

    /*
     * GTE.
     */
    InitGeom();

    gte_SetGeomOffset(
        SCREEN_XRES / 2,
        SCREEN_YRES / 2
    );

    gte_SetGeomScreen(256);
}

/* ============================================================
   TEXTURA
   ============================================================ */

static void upload_texture(void) {

    RECT rect_tex;

    /*
     * IMPORTANTE:
     *
     * 4 bits por pixel.
     *
     * Uma palavra de 16 bits contém 4 pixels.
     *
     * 128 / 4 = 32 palavras por linha.
     */
    rect_tex.x = TEX_VRAM_X;
    rect_tex.y = TEX_VRAM_Y;

    rect_tex.w = NESTON_TEX_W / 4;
    rect_tex.h = NESTON_TEX_H;

    LoadImage(
        &rect_tex,
        (uint32_t *)neston_tex_bytes
    );

    /*
     * Espera a transferência da textura.
     */
    DrawSync(0);

    /*
     * CLUT de 16 cores.
     */
    RECT rect_clut;

    rect_clut.x = CLUT_VRAM_X;
    rect_clut.y = CLUT_VRAM_Y;

    rect_clut.w = 16;
    rect_clut.h = 1;

    LoadImage(
        &rect_clut,
        (uint32_t *)neston_clut
    );

    /*
     * Espera a CLUT.
     */
    DrawSync(0);
}

/* ============================================================
   ÁUDIO
   ============================================================ */

/*
 * Dispara a transferência do ADPCM pra RAM da SPU
 * SEM esperar terminar. A DMA da SPU é um canal
 * separado da DMA da GPU (usada no upload_texture),
 * então as duas podem rodar ao mesmo tempo — é só
 * não bloquear aqui.
 */
static void init_sound_start(void) {

    /*
     * Inicializa SPU.
     */
    SpuInit();

    /*
     * Usa DMA.
     */
    SpuSetTransferMode(
        SPU_TRANSFER_BY_DMA
    );

    /*
     * Para a voz antes da configuração.
     */
    SpuSetKey(
        0,
        1 << 0
    );

    /*
     * DEFINE EXPLICITAMENTE o endereço
     * da transferência na RAM do SPU.
     */
    SpuSetTransferStartAddr(
        SPU_SPRS_ADDR
    );

    /*
     * Envia o ADPCM (não bloqueia).
     */
    SpuWrite(
        (const uint32_t *)audio_adpcm_data,
        AUDIO_ADPCM_SIZE
    );
}

/*
 * Espera a transferência (que já rodou em paralelo
 * com upload_texture) terminar e liga a voz.
 */
static void init_sound_finish(void) {

    /*
     * Espera a DMA terminar. Como foi disparada antes
     * do upload_texture, na prática já deve estar
     * pronta ou faltar bem pouco quando chegamos aqui.
     */
    SpuIsTransferCompleted(
        SPU_TRANSFER_WAIT
    );

    /*
     * Master volume um pouco mais baixo (evita o
     * áudio sair estourado no hardware real e em
     * emuladores mais precisos).
     */
    SpuSetCommonMasterVolume(0x2000, 0x2000);

    /*
     * Volume da voz bem mais baixo.
     * 0x3FFF = máximo
     * 0x1800 = antigo (ainda estourava)
     * 0x0A00 ≈ 15% — bem mais civilizado
     */
    SpuSetVoiceVolume(
        0,
        0x0A00,
        0x0A00
    );

    /*
     * Pitch.
     */
    SpuSetVoicePitch(
        0,
        0x1000
    );

    /*
     * Endereço do sample.
     */
    SpuSetVoiceStartAddr(
        0,
        SPU_SPRS_ADDR
    );

    /*
     * Começa a tocar.
     */
    SpuSetKey(
        1,
        1 << 0
    );
}

/* ============================================================
   MAIN
   ============================================================ */

int main(void) {

    /*
     * Inicialização.
     */
    init_graphics();

    /*
     * Dispara a DMA do som primeiro e deixa ela rodar
     * em paralelo com o upload da textura (DMA da GPU),
     * em vez de esperar uma terminar pra começar a outra.
     */
    init_sound_start();

    upload_texture();

    init_sound_finish();

    /*
     * Texture page:
     *
     * 0 = 4-bit
     * 0 = sem semi-transparência
     */
    uint16_t tpage = getTPage(
        0,
        0,
        TEX_VRAM_X,
        TEX_VRAM_Y
    );

    /*
     * CLUT.
     */
    uint16_t clut = getClut(
        CLUT_VRAM_X,
        CLUT_VRAM_Y
    );

    /*
     * Liga display.
     */
    SetDispMask(1);

    while (1) {

        /*
         * Alterna framebuffer.
         */
        g_active_buffer ^= 1;

        RenderBuffer *rb =
            &g_buffers[g_active_buffer];

        uint8_t *prim_ptr =
            g_prim_buffer[g_active_buffer];

        g_prim_buffer_offset = 0;

        /*
         * Limpa Ordering Table.
         */
        ClearOTagR(
            rb->ot,
            OT_LENGTH
        );

        /*
         * Rotação.
         */
        g_rotation.vx += 12;
        g_rotation.vy += 16;
        g_rotation.vz += 8;

        /*
         * Matriz.
         */
        MATRIX transform;

        RotMatrix(
            &g_rotation,
            &transform
        );

        TransMatrix(
            &transform,
            &g_translation
        );

        gte_SetRotMatrix(
            &transform
        );

        gte_SetTransMatrix(
            &transform
        );

        /*
         * Seis faces.
         */
        for (int i = 0; i < 6; i++) {

            /*
             * Proteção.
             */
            if (
                g_prim_buffer_offset +
                sizeof(POLY_FT4) >
                PRIM_BUFFER_SIZE
            ) {
                break;
            }

            POLY_FT4 *poly =
                (POLY_FT4 *)(
                    prim_ptr +
                    g_prim_buffer_offset
                );

            /*
             * Inicializa.
             */
            setPolyFT4(poly);

            /*
             * Cor neutra.
             *
             * Isso é importante para a textura
             * não ficar artificialmente escurecida.
             */
            setRGB0(
                poly,
                128,
                128,
                128
            );

            /*
             * UV.
             */
            setUV4(
                poly,

                g_uv_coords[0].vx,
                g_uv_coords[0].vy,

                g_uv_coords[1].vx,
                g_uv_coords[1].vy,

                g_uv_coords[2].vx,
                g_uv_coords[2].vy,

                g_uv_coords[3].vx,
                g_uv_coords[3].vy
            );

            /*
             * Texture page + CLUT.
             */
            poly->tpage = tpage;
            poly->clut = clut;

            long flag;
            long otz;

            /*
             * Transformação.
             */
            long nclip =
                my_RotTransPers4(
                    &g_cube_vertices[
                        g_cube_indices[i][0]
                    ],

                    &g_cube_vertices[
                        g_cube_indices[i][1]
                    ],

                    &g_cube_vertices[
                        g_cube_indices[i][2]
                    ],

                    &g_cube_vertices[
                        g_cube_indices[i][3]
                    ],

                    (long *)&poly->x0,
                    (long *)&poly->x1,
                    (long *)&poly->x2,
                    (long *)&poly->x3,

                    &otz,
                    &flag
                );

            /*
             * Só adiciona faces visíveis.
             */
            if (
                nclip > 0 &&
                otz > 0 &&
                otz < OT_LENGTH
            ) {

                g_prim_buffer_offset +=
                    sizeof(POLY_FT4);

                addPrim(
                    &rb->ot[otz],
                    poly
                );
            }
        }

        /*
         * Sincroniza frame.
         */
        VSync(0);

        PutDispEnv(
            &rb->disp
        );

        PutDrawEnv(
            &rb->draw
        );

        /*
         * Renderiza.
         */
        DrawOTag(
            (uint32_t *)&rb->ot[
                OT_LENGTH - 1
            ]
        );
    }

    return 0;
}
