/* Host-neutral, single-board lifecycle. No windows, devices or pacing.
 * This API is not thread-safe. Only one board may be loaded at a time. */
#ifndef NEOGEORECOMP_RUNTIME_H
#define NEOGEORECOMP_RUNTIME_H

#include <neogeorecomp/neogeo.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NG_RUNTIME_FPS (12000000.0 / (768.0 * 264.0))
#define NG_RUNTIME_AUDIO_RATE (4000000.0 / 72.0)

typedef struct {
    const char *rom_path;       /* required; never resolved relative to a hidden cwd */
    const char *bios_path;      /* NULL: same directory as the game ROMs */
    const char *bios_name;      /* NULL: the game's default BIOS */
    int screen_width;          /* 0: 320; otherwise even, 320..496; reload to change */
    int interpreter_only;
    int verify;
} ng_runtime_options_t;

typedef struct {
    const uint32_t *pixels;     /* 0xFFRRGGBB; valid until next frame/reset/unload */
    unsigned width, height;
    size_t pitch;              /* bytes between rows */
    const int16_t *audio;      /* interleaved signed 16-bit stereo */
    size_t audio_frames;       /* pairs, not individual samples */
} ng_runtime_frame_t;

int ng_runtime_load(const ng_game_t *game, const ng_func_entry_t *funcs,
                    size_t nfuncs, const ng_runtime_options_t *options);
int ng_runtime_run_frame(const ng_input_t *input, ng_runtime_frame_t *output);
void ng_runtime_reset(void);
void ng_runtime_unload(void);  /* safe after errors and repeated calls */
void ng_runtime_counters(uint64_t *native_blocks, uint64_t *interpreted_instructions);

#ifdef __cplusplus
}
#endif
#endif
