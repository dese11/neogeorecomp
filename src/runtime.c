/* Board ownership and the scanline scheduler, independent of the frontend. */
#include "ng_internal.h"
#include "romload.h"
#include "ast.h"
#include <neogeorecomp/bus.h>
#include <neogeorecomp/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t ng_frame_start;
static uint32_t s_fb[NG_MAX_SCREEN_W * NG_SCREEN_H];
static int16_t s_samples[4096 * 2];
static struct {
    const ng_game_t *game;
    uint8_t *prom, *bios, *sfix, *sm1, *lo, *crom, *srom, *m1;
    uint8_t *vrom[NG_MAX_PARTS];
    int loaded, mvs;
} s_board;

void ng_runtime_unload(void) {
    ast_close();
    video_set_extend(NULL);
    exec_shutdown();
    bus_shutdown();
    video_shutdown();
    audio_shutdown();
    if (s_board.game && s_board.game->unload) s_board.game->unload();
    free(s_board.prom); free(s_board.bios); free(s_board.sfix); free(s_board.sm1);
    free(s_board.lo); free(s_board.crom); free(s_board.srom); free(s_board.m1);
    for (int i = 0; i < NG_MAX_PARTS; i++) free(s_board.vrom[i]);
    memset(&s_board, 0, sizeof(s_board));
    memset(s_fb, 0, sizeof(s_fb));
    memset(s_samples, 0, sizeof(s_samples));
    ng_cycles = ng_frame_start = 0;
    ng_irq_pending = 0;
    ng_screen_w = NG_SCREEN_W;
    io_reset(1);
}

void ng_runtime_set_soundtrack_volume(unsigned volume) { ast_set_volume(volume); }

void ng_runtime_reset(void) {
    if (!s_board.loaded) return;
    ast_reset();
    ng_cycles = ng_frame_start = 0;
    memset(s_fb, 0, sizeof(s_fb));
    memset(s_samples, 0, sizeof(s_samples));
    bus_reset();
    lspc_reset();
    io_reset(s_board.mvs);
    pal_select_bank(0);
    video_set_fix_bios(s_board.mvs);
    audio_select_bios_rom(s_board.mvs);
    audio_reset();
    audio_clear_samples();
    exec_reset();
    ng_irq_pending = 4;
}

int ng_runtime_load(const ng_game_t *game, const ng_func_entry_t *funcs,
                    size_t nfuncs, const ng_runtime_options_t *o) {
    if (s_board.loaded || !game || !o || !o->rom_path || (nfuncs && !funcs)) return -1;
    int width = o->screen_width ? o->screen_width : NG_SCREEN_W;
    if (width < NG_SCREEN_W || width > NG_MAX_SCREEN_W || (width & 1)) {
        fprintf(stderr, "error: output width must be even and between 320 and 496\n");
        return -1;
    }
    const char *bios_path = o->bios_path ? o->bios_path : o->rom_path;
    const char *bios_name = o->bios_name ? o->bios_name : game->bios ? game->bios : "sp-s2.sp1";
    size_t bios_sz = 0, sfix_sz = 0, sm1_sz = 0, lo_sz = 0, srom_sz = 0, m1_sz = 0;
    size_t vsizes[NG_MAX_PARTS] = {0};
    const uint8_t *vroms[NG_MAX_PARTS] = {0};
    int nv = 0;
    s_board.game = game;
    s_board.prom = ng_load_prom(o->rom_path, game);
    if (!s_board.prom) goto fail;
    s_board.bios = ng_read_file(bios_path, bios_name, &bios_sz);
    s_board.sfix = ng_read_file(bios_path, "sfix.sfix", &sfix_sz);
    s_board.sm1 = ng_read_file(bios_path, "sm1.sm1", &sm1_sz);
    s_board.lo = ng_read_file(bios_path, "000-lo.lo", &lo_sz);
    if (!s_board.bios || !s_board.sfix || !s_board.sm1 || !s_board.lo) goto fail;
    if (bios_sz < 0x20000 || sfix_sz < 0x20000 || sm1_sz < 0x20000 || lo_sz < 0x10000) {
        fprintf(stderr, "error: truncated system ROM set (BIOS/SFIX/SM1/zoom)\n");
        goto fail;
    }
    s_board.crom = ng_load_crom(o->rom_path, game);
    s_board.srom = ng_read_file(o->rom_path, game->srom, &srom_sz);
    s_board.m1 = ng_read_file(o->rom_path, game->m1, &m1_sz);
    if (!s_board.crom || !s_board.srom || !s_board.m1 || srom_sz < 32 || !m1_sz) goto fail;
    for (; nv < NG_MAX_PARTS && game->vrom[nv]; nv++) {
        s_board.vrom[nv] = ng_read_file(o->rom_path, game->vrom[nv], &vsizes[nv]);
        if (!s_board.vrom[nv]) goto fail;
        vroms[nv] = s_board.vrom[nv];
    }
    ng_swap16(s_board.bios, 0x20000);
    ng_screen_w = width;
    if (game->patch) game->patch(s_board.prom, game->prom_size, width);
    s_board.mvs = !game->aes;
    ng_board_t board = {s_board.prom, game->prom_size, s_board.bios, s_board.mvs};
    bus_init(&board);
    ng_cycles = ng_frame_start = 0;
    if (video_load(s_board.crom, game->crom_size, s_board.srom, srom_sz, s_board.sfix, s_board.lo)) goto fail;
    video_set_extend(width > NG_SCREEN_W ? game->extend : NULL);
    free(s_board.crom); s_board.crom = NULL;
    if (audio_init(s_board.m1, m1_sz, s_board.sm1, vroms, vsizes, nv)) goto fail;
    if (ast_open(o->soundtrack_path, game->name, s_board.m1, m1_sz, o->soundtrack_volume)) goto fail;
    if (exec_init(funcs, nfuncs, o->interpreter_only || !nfuncs)) goto fail;
    exec_set_verify(o->verify);
    s_board.loaded = 1;
    ng_runtime_reset();
    fprintf(stderr, "[neogeorecomp] %s: %zu recompiled entry points%s\n", game->name, nfuncs,
            (o->interpreter_only || !nfuncs) ? ", interpreter only" : "");
    return 0;
fail:
    fprintf(stderr, "error: cannot load ROM set; see docs/running.md\n");
    ng_runtime_unload();
    return -1;
}

static void run_until(int64_t t) {
    while (ng_cycles < t) {
        int64_t target = t, d = lspc_timer_deadline();
        if (d < target) target = d;
        if (target > ng_cycles) exec_run(target);
        if (ng_cycles >= lspc_timer_deadline()) lspc_timer_fire();
    }
}

int ng_runtime_run_frame(const ng_input_t *input, ng_runtime_frame_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!s_board.loaded) return -1;
    io_set_inputs(input ? input : &(ng_input_t){0});
    int64_t fs = ng_frame_start;
    for (int line = 0; line < NG_LINES_PER_FRAME; line++) {
        int64_t ls = fs + (int64_t)line * NG_CLOCKS_PER_LINE;
        run_until(ls);
        if (line >= NG_FIRST_VISIBLE && line < NG_VBLANK_LINE)
            video_render_line(line - NG_FIRST_VISIBLE, s_fb + (line - NG_FIRST_VISIBLE) * ng_screen_w);
        if (line == NG_VBLANK_LINE) {
            run_until(ls + NG_VBLANK_HPOS * 2);
            lspc_vblank();
        }
    }
    ng_frame_start = fs + NG_CLOCKS_PER_FRAME;
    run_until(ng_frame_start);
    audio_sync();
    out->pixels = s_fb;
    out->width = (unsigned)ng_screen_w;
    out->height = NG_SCREEN_H;
    out->pitch = (size_t)ng_screen_w * sizeof(*s_fb);
    out->audio = s_samples;
    out->audio_frames = audio_take_samples(s_samples, 4096);
    return 0;
}

void ng_runtime_counters(uint64_t *native, uint64_t *interpreted) {
    if (native) *native = exec_native_blocks;
    if (interpreted) *interpreted = exec_interp_instrs;
}
