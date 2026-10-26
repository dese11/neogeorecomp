/*
 * neogeo.c — ng_main(): SDL/headless command line and host frame loop.
 * Board ownership and scanline timing live in runtime.c.
 *
 * Input comes from the window, or from an --input script so headless runs
 * are reproducible: `<frame>[-<end>] <button>[+<button>...]`, one per line.
 */
#include "ng_internal.h"
#include <neogeorecomp/bus.h>
#include <neogeorecomp/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- options ---- */

#define MAX_SHOTS 64
typedef struct {
    const char *rom_path, *bios_path, *record, *input, *dump_misses, *bios_name, *wav;
    int headless, interp, verify, scale, mute, width_set;
    long frames;
    struct { long frame; const char *path; } shots[MAX_SHOTS];
    int nshots;
} options_t;

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --rom-path DIR        game ROM files (default: roms)\n"
        "  --bios-path DIR       system ROM files (default: the ROM path)\n"
        "  --bios FILE           system ROM (default: the game's, else sp-s2.sp1, MVS Asia/Europe)\n"
        "  --headless            no window; run as fast as possible\n"
        "  --record FILE.mp4     pipe frames to ffmpeg\n"
        "  --frames N            stop after N frames\n"
        "  --screenshot N:FILE   save frame N as PNG (repeatable)\n"
        "  --input FILE          scripted input (see docs/running.md)\n"
        "  --interp              run everything in the interpreter\n"
        "  --verify              re-run every recompiled block in the interpreter and compare\n"
        "  --dump-misses FILE    write interpreted entry points for the recompiler\n"
        "  --scale N             window scale (default 3)\n"
        "  --width N             output width, 320-%d: margins show what lies just off screen\n"
        "  --widescreen          --width 400 (16:9 with square pixels)\n"
        "  --wav FILE            write the audio to a WAV file\n"
        "  --mute                no sound from the window\n", prog, NG_MAX_SCREEN_W);
}

static int parse_args(int argc, char **argv, options_t *o) {
    memset(o, 0, sizeof(*o));
    o->rom_path = "roms"; o->scale = 3; o->frames = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--rom-path") && next) { o->rom_path = next; i++; }
        else if (!strcmp(a, "--bios-path") && next) { o->bios_path = next; i++; }
        else if (!strcmp(a, "--bios") && next) { o->bios_name = next; i++; }
        else if (!strcmp(a, "--headless")) o->headless = 1;
        else if (!strcmp(a, "--record") && next) { o->record = next; i++; }
        else if (!strcmp(a, "--frames") && next) { o->frames = atol(next); i++; }
        else if (!strcmp(a, "--input") && next) { o->input = next; i++; }
        else if (!strcmp(a, "--interp")) o->interp = 1;
        else if (!strcmp(a, "--verify")) o->verify = 1;
        else if (!strcmp(a, "--dump-misses") && next) { o->dump_misses = next; i++; }
        else if (!strcmp(a, "--scale") && next) { o->scale = atoi(next); i++; }
        else if (!strcmp(a, "--width") && next) { ng_screen_w = atoi(next); o->width_set = 1; i++; }
        else if (!strcmp(a, "--widescreen")) { ng_screen_w = 400; o->width_set = 1; }
        else if (!strcmp(a, "--wav") && next) { o->wav = next; i++; }
        else if (!strcmp(a, "--mute")) o->mute = 1;
        else if (!strcmp(a, "--screenshot") && next) {
            const char *colon = strchr(next, ':');
            if (!colon || o->nshots == MAX_SHOTS) { usage(argv[0]); return -1; }
            o->shots[o->nshots].frame = atol(next);
            o->shots[o->nshots].path = colon + 1;
            o->nshots++; i++;
        }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); exit(0); }
        else { usage(argv[0]); return -1; }
    }
    if (!o->bios_path) o->bios_path = o->rom_path;
    /* even, so both margins are equal */
    if (ng_screen_w < NG_SCREEN_W || ng_screen_w > NG_MAX_SCREEN_W || (ng_screen_w & 1)) {
        fprintf(stderr, "error: --width must be an even number from %d to %d\n", NG_SCREEN_W, NG_MAX_SCREEN_W);
        return -1;
    }
    return 0;
}

/* ---- scripted input ---- */

typedef struct { long start, end; ng_input_t in; } script_ev_t;
static script_ev_t *s_script;
static int s_nscript;

static int button(const char *name, ng_input_t *in) {
    static const struct { const char *n; int kind; uint8_t bit; } map[] = {
        {"up", 1, NG_UP}, {"down", 1, NG_DOWN}, {"left", 1, NG_LEFT}, {"right", 1, NG_RIGHT},
        {"a", 1, NG_A}, {"b", 1, NG_B}, {"c", 1, NG_C}, {"d", 1, NG_D},
        {"p2up", 2, NG_UP}, {"p2down", 2, NG_DOWN}, {"p2left", 2, NG_LEFT}, {"p2right", 2, NG_RIGHT},
        {"p2a", 2, NG_A}, {"p2b", 2, NG_B}, {"p2c", 2, NG_C}, {"p2d", 2, NG_D},
        {"start", 3, 0}, {"start2", 4, 0}, {"select", 5, 0}, {"coin", 6, 0}, {"coin2", 7, 0},
        {"service", 8, 0}, {"test", 9, 0},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(name, map[i].n)) continue;
        switch (map[i].kind) {
        case 1: in->p1 |= map[i].bit; break;
        case 2: in->p2 |= map[i].bit; break;
        case 3: in->start1 = 1; break;
        case 4: in->start2 = 1; break;
        case 5: in->select1 = 1; break;
        case 6: in->coin1 = 1; break;
        case 7: in->coin2 = 1; break;
        case 8: in->service = 1; break;
        case 9: in->test = 1; break;
        }
        return 0;
    }
    return -1;
}

static int load_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "error: cannot open %s\n", path); return -1; }
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        long start, end;
        char buttons[200];
        int n = sscanf(line, "%ld-%ld %199s", &start, &end, buttons);
        if (n != 3) {
            if (sscanf(line, "%ld %199s", &start, buttons) != 2) continue;
            end = start + 5;   /* long enough for the BIOS to debounce a press */
        }
        script_ev_t ev = {start, end, {0}};
        for (char *tok = strtok(buttons, "+"); tok; tok = strtok(NULL, "+"))
            if (button(tok, &ev.in)) {
                fprintf(stderr, "%s:%d: unknown button '%s'\n", path, lineno, tok);
                fclose(f); return -1;
            }
        s_script = (script_ev_t *)realloc(s_script, (size_t)(s_nscript + 1) * sizeof(*s_script));
        s_script[s_nscript++] = ev;
    }
    fclose(f);
    return 0;
}

static void script_input(long frame, ng_input_t *in) {
    for (int i = 0; i < s_nscript; i++) {
        const script_ev_t *e = &s_script[i];
        if (frame < e->start || frame > e->end) continue;
        in->p1 |= e->in.p1; in->p2 |= e->in.p2;
        in->start1 |= e->in.start1; in->start2 |= e->in.start2; in->select1 |= e->in.select1;
        in->coin1 |= e->in.coin1; in->coin2 |= e->in.coin2;
        in->service |= e->in.service; in->test |= e->in.test;
    }
}

int ng_main(int argc, char **argv, const ng_game_t *game,
            const ng_func_entry_t *funcs, size_t nfuncs) {
    options_t o;
    platform_early_init();
    if (parse_args(argc, argv, &o)) return 2;
    if (o.input && load_script(o.input)) return 1;
    /* The window's saved settings; the command line wins. Headless runs
     * never read them, so they behave the same everywhere. */
    if (!o.headless) {
        int w = window_settings_load(game->name);
        if (!o.width_set && w >= NG_SCREEN_W && w <= NG_MAX_SCREEN_W && !(w & 1)) ng_screen_w = w;
    }
    if (!o.bios_name) o.bios_name = game->bios ? game->bios : "sp-s2.sp1";

    ng_runtime_options_t runtime = {
        .rom_path = o.rom_path, .bios_path = o.bios_path, .bios_name = o.bios_name,
        .screen_width = ng_screen_w, .interpreter_only = o.interp, .verify = o.verify
    };
    if (ng_runtime_load(game, funcs, nfuncs, &runtime)) {
        free(s_script); s_script = NULL; s_nscript = 0;
        return 1;
    }
    int result = 1;
    if (platform_init(game->title, o.scale, o.headless, o.record)) goto cleanup;
    if (o.mute) platform_mute();
    if (o.wav && platform_wav(o.wav)) goto cleanup;
    ng_runtime_frame_t output = {0};
    for (long frame = 0; o.frames < 0 || frame < o.frames; frame++) {
        ng_input_t in;
        memset(&in, 0, sizeof(in));
        if (!platform_frame(output.pixels, &in)) break;
        if (window_take_reset()) ng_runtime_reset();
        script_input(frame, &in);
        if (ng_runtime_run_frame(&in, &output)) goto cleanup;
        platform_audio(output.audio, output.audio_frames);
        for (int i = 0; i < o.nshots; i++)
            if (o.shots[i].frame == frame) {
                if (write_png(o.shots[i].path, output.pixels, (int)output.width, (int)output.height))
                    fprintf(stderr, "error: cannot write %s\n", o.shots[i].path);
                else
                    fprintf(stderr, "[neogeorecomp] frame %ld -> %s\n", frame, o.shots[i].path);
            }
    }
    platform_frame(output.pixels, &(ng_input_t){0});

    exec_report();
    if (o.dump_misses) exec_dump_misses(o.dump_misses);
    result = 0;
cleanup:
    platform_shutdown();
    ng_runtime_unload();
    free(s_script); s_script = NULL; s_nscript = 0;
    return result;
}
