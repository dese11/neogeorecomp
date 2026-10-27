/* Internal, canonical little-endian state stream. No host structures/pointers. */
#ifndef NG_STATE_H
#define NG_STATE_H
#include <stdint.h>
#include <stddef.h>
enum { NGS_COUNT, NGS_WRITE, NGS_CHECK, NGS_READ };
typedef struct { uint8_t *data; size_t size, pos; int mode, error; } ng_state_t;
#ifdef __cplusplus
extern "C" {
#endif
void ngs_bytes(ng_state_t *s, void *data, size_t n);
uint64_t ngs_number(ng_state_t *s, uint64_t value, unsigned bytes);
uint8_t ngs_u8(ng_state_t *s, uint8_t *v);
uint16_t ngs_u16(ng_state_t *s, uint16_t *v);
uint32_t ngs_u32(ng_state_t *s, uint32_t *v);
uint64_t ngs_u64(ng_state_t *s, uint64_t *v);
int64_t ngs_i64(ng_state_t *s, int64_t *v);
int ngs_int(ng_state_t *s, int *v);
size_t ngs_size(ng_state_t *s, size_t *v);
void ngs_require(ng_state_t *s, int condition);
uint32_t ngs_crc32(const void *data, size_t size);
uint64_t ngs_hash64(uint64_t hash, const void *data, size_t size);
#define NGS_HASH_INIT UINT64_C(14695981039346656037)
void bus_state(ng_state_t *s);
void exec_state(ng_state_t *s);
void lspc_state(ng_state_t *s);
void io_state(ng_state_t *s);
void video_state(ng_state_t *s);
void audio_state(ng_state_t *s);
void ast_state(ng_state_t *s);
void ast_state_cancel(void);
uint64_t ast_identity(void);
#ifdef __cplusplus
}
#endif
#endif
