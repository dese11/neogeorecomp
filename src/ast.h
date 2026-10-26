#ifndef NG_AST_H
#define NG_AST_H
#include <stdint.h>
#include <stddef.h>
/* Local PCM soundtrack packs. No ROM patches and no host audio devices. */
int ast_open(const char *path, const char *game, const uint8_t *m1, size_t size, unsigned volume);
void ast_close(void);
void ast_reset(void);
int ast_enabled(void);
void ast_set_volume(unsigned volume);
/* Returns 1 when the driver should receive its music-stop command instead.
 * Effects, system commands and their argument bytes return 0. */
int ast_command(uint8_t command);
void ast_mix(int16_t *stereo, size_t frames);
unsigned ast_current_track(void);
#endif
