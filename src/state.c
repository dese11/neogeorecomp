#include "state.h"
#include <string.h>
#include <limits.h>

static int room(ng_state_t *s, size_t n) {
    if (s->pos>s->size || n>s->size-s->pos) { s->error=1; return 0; }
    return !s->error;
}
void ngs_bytes(ng_state_t *s, void *data, size_t n) {
    if (!room(s,n)) return;
    if (s->mode==NGS_WRITE) memcpy(s->data+s->pos,data,n);
    else if (s->mode==NGS_READ) memcpy(data,s->data+s->pos,n);
    s->pos+=n;
}
uint64_t ngs_number(ng_state_t *s, uint64_t value, unsigned n) {
    if (!room(s,n)) return 0;
    if (s->mode==NGS_READ || s->mode==NGS_CHECK) {
        value=0;
        for (unsigned i=0;i<n;i++) value|=(uint64_t)s->data[s->pos+i]<<(i*8);
    } else if (s->mode==NGS_WRITE)
        for (unsigned i=0;i<n;i++) s->data[s->pos+i]=(uint8_t)(value>>(i*8));
    s->pos+=n; return value;
}
#define NUMBER(bits, bytes) \
uint##bits##_t ngs_u##bits(ng_state_t *s, uint##bits##_t *v) { \
    uint##bits##_t n=(uint##bits##_t)ngs_number(s,*v,bytes); \
    if (s->mode==NGS_READ && !s->error) { *v=n; } return n; }
NUMBER(8,1)
NUMBER(16,2)
NUMBER(32,4)
NUMBER(64,8)
#undef NUMBER
int64_t ngs_i64(ng_state_t *s, int64_t *v) {
    uint64_t bits; memcpy(&bits,v,8); bits=ngs_number(s,bits,8);
    int64_t n; memcpy(&n,&bits,8);
    if (s->mode==NGS_READ && !s->error) *v=n;
    return n;
}
int ngs_int(ng_state_t *s, int *v) {
    uint32_t bits=(uint32_t)*v; bits=(uint32_t)ngs_number(s,bits,4);
    int32_t n; memcpy(&n,&bits,4);
    if (s->mode==NGS_READ && !s->error) *v=n;
    return n;
}
size_t ngs_size(ng_state_t *s, size_t *v) {
    uint64_t n=ngs_number(s,(uint64_t)*v,4);
    if (s->mode==NGS_READ && !s->error) *v=(size_t)n;
    return (size_t)n;
}
void ngs_require(ng_state_t *s, int valid) { if (!valid) s->error=1; }
uint32_t ngs_crc32(const void *data, size_t n) {
    /* Nibble table avoids lazy global initialization and bytewise bit loops. */
    static const uint32_t t[16]={0,0x1db71064,0x3b6e20c8,0x26d930ac,0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,
        0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c};
    const uint8_t *p=data; uint32_t c=UINT32_MAX;
    while (n--) { c^=*p++; c=(c>>4)^t[c&15]; c=(c>>4)^t[c&15]; }
    return ~c;
}
uint64_t ngs_hash64(uint64_t h, const void *data, size_t n) {
    const uint8_t *p=data;
    while (n--) { h^=*p++; h*=UINT64_C(1099511628211); }
    return h;
}
