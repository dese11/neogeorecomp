/* Optional arranged music mixed into the existing YM2610 sample clock.
 * Packs are prepared locally from the user's disc, never embedded in code. */
#include "ast.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define CACHE_FRAMES 4096
#define PHASE_RATE UINT64_C(4000000)
#define PHASE_STEP UINT64_C(3175200) /* 44100 * 72 -> YM2610 rate 4000000/72 */
typedef struct {
    char file[1200];
    uint32_t offset, frames;
    unsigned number, loop;
} track_t;
static track_t s_tracks[256];
static uint8_t s_native[256], s_args[256];
static FILE *s_file, *s_prepared;
static uint64_t s_pack_identity, s_track_identity[256];
static int s_enabled, s_track = -1;
static unsigned s_volume, s_pending, s_owner;
static uint64_t s_phase, s_fade_left, s_fade_total;
static uint32_t s_gain = 65536, s_fade_gain;
static uint8_t s_cache[CACHE_FRAMES * 4];
static uint32_t s_cache_at, s_cache_count;

static uint32_t le32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = UINT32_MAX;
    while (n--) {
        c ^= *p++;
        for (int bit=0; bit<8; bit++) c = (c>>1) ^ (0xedb88320u & (0u-(c&1)));
    }
    return ~c;
}

static int wav_info(track_t *t) {
    FILE *f = fopen(t->file, "rb");
    if (!f) return -1;
    uint8_t h[16];
    int valid = fread(h,1,12,f)==12 && !memcmp(h,"RIFF",4) && !memcmp(h+8,"WAVE",4);
    long end = 0;
    if (valid && !fseek(f,0,SEEK_END)) end = ftell(f);
    if (end < 44 || fseek(f,12,SEEK_SET)) valid = 0;
    int format = 0;
    while (valid && ftell(f) <= end-8 && fread(h,1,8,f)==8) {
        uint32_t size = le32(h+4);
        long at = ftell(f);
        if (size > (uint64_t)(end-at)) { valid=0; break; }
        if (!memcmp(h,"fmt ",4)) {
            if (size<16 || fread(h,1,16,f)!=16) { valid=0; break; }
            format = le16(h)==1 && le16(h+2)==2 && le32(h+4)==44100 &&
                     le32(h+8)==176400 && le16(h+12)==4 && le16(h+14)==16;
        } else if (!memcmp(h,"data",4)) {
            if (size && !(size%4) && at>=0 && (uint64_t)at<=UINT32_MAX) {
                t->offset=(uint32_t)at; t->frames=size/4;
            }
        }
        if (size > LONG_MAX-at-1 || fseek(f,at+size+(size&1),SEEK_SET)) { valid=0; break; }
    }
    fclose(f);
    return valid && format && t->frames ? 0 : -1;
}

static void stop(void) {
    if (s_file) fclose(s_file);
    s_file=NULL; s_track=-1; s_phase=0; s_cache_count=0;
    s_gain=65536; s_fade_left=s_fade_total=0; s_fade_gain=0;
}
void ast_reset(void) { stop(); s_pending=s_owner=0; }
void ast_close(void) {
    ast_state_cancel(); s_pack_identity=0;
    memset(s_track_identity,0,sizeof(s_track_identity));
    ast_reset(); s_enabled=0;
    memset(s_tracks,0,sizeof(s_tracks));
    memset(s_native,0,sizeof(s_native)); memset(s_args,0,sizeof(s_args));
}
void ast_set_volume(unsigned volume) { if (volume<=100) s_volume=volume; }
int ast_enabled(void) { return s_enabled; }
unsigned ast_current_track(void) { return s_track<0 ? 0 : s_tracks[s_track].number; }

uint64_t ast_identity(void) { return s_pack_identity; }
static uint64_t file_identity(const char *path) {
    FILE *f=fopen(path,"rb"); if (!f) return 0;
    uint64_t h=NGS_HASH_INIT; uint8_t buf[65536]; size_t n;
    while ((n=fread(buf,1,sizeof(buf),f))) h=ngs_hash64(h,buf,n);
    int bad=ferror(f); fclose(f); return bad ? 0 : h;
}

int ast_open(const char *path, const char *game, const uint8_t *m1, size_t size, unsigned volume) {
    ast_close();
    if (!path) return 0;
    char name[1400], line[1400], pack_game[64];
    int len=snprintf(name,sizeof(name),"%s/soundtrack.txt",path);
    if (len<0 || (size_t)len>=sizeof(name) || volume>100) return -1;
    FILE *f=fopen(name,"r");
    if (!f) { fprintf(stderr,"error: cannot open AST manifest %s\n",name); return -1; }
    unsigned version, expected_crc;
    int ok=fgets(line,sizeof(line),f) &&
        sscanf(line,"NGAST %u %63s %x",&version,pack_game,&expected_crc)==3 && version==1 &&
        !strcmp(pack_game,game) && !strcmp(game,"kof98") && m1 && crc32(m1,size)==expected_crc;
    unsigned count=0;
    while (ok && fgets(line,sizeof(line),f)) {
        char kind[24], file[128], extra;
        unsigned command, number, loop;
        if (line[0]=='#' || line[0]=='\n' || line[0]=='\r') continue;
        if (sscanf(line,"%23s %x",kind,&command)!=2 || command>255) { ok=0; break; }
        if (!strcmp(kind,"music")) {
            if (sscanf(line,"%*s %x %u %u %127s %c",&command,&number,&loop,file,&extra)!=4 ||
                number<2 || number>99 || loop>1 || strchr(file,'/') || strchr(file,'\\') ||
                strstr(file,"..") || s_tracks[command].number || s_native[command]) { ok=0; break; }
            track_t *t=&s_tracks[command];
            len=snprintf(t->file,sizeof(t->file),"%s/%s",path,file);
            t->number=number; t->loop=loop;
            if (len<0 || (size_t)len>=sizeof(t->file) || wav_info(t)) {
                fprintf(stderr,"error: AST needs stereo PCM16 44100 Hz WAV: %s\n",t->file);
                ok=0; break;
            }
            for (unsigned i=0;i<command;i++)
                if (!strcmp(s_tracks[i].file,t->file)) { s_track_identity[command]=s_track_identity[i]; break; }
            if (!s_track_identity[command]) s_track_identity[command]=file_identity(t->file);
            if (!s_track_identity[command]) { ok=0; break; }
            count++;
        } else if (!strcmp(kind,"native")) {
            if (s_tracks[command].number || s_native[command]) { ok=0; break; }
            s_native[command]=1;
        } else if (!strcmp(kind,"arg")) {
            if (sscanf(line,"%*s %x %u %c",&command,&number,&extra)!=2 || number<1 || number>2) { ok=0; break; }
            s_args[command]=(uint8_t)number;
        } else { ok=0; break; }
    }
    if (ferror(f)) ok=0;
    fclose(f);
    if (!ok || !count) {
        fprintf(stderr,"error: invalid AST pack or incompatible sound ROM: %s\n",name);
        ast_close(); return -1;
    }
    s_pack_identity=NGS_HASH_INIT;
    s_pack_identity=ngs_hash64(s_pack_identity,s_native,sizeof(s_native));
    s_pack_identity=ngs_hash64(s_pack_identity,s_args,sizeof(s_args));
    for (unsigned i=0;i<256;i++) {
        uint8_t bytes[24]; ng_state_t state={bytes,sizeof(bytes),0,NGS_WRITE,0};
        ngs_number(&state,s_tracks[i].number,4); ngs_number(&state,s_tracks[i].loop,4);
        ngs_number(&state,s_tracks[i].frames,4); ngs_number(&state,s_tracks[i].offset,4);
        ngs_number(&state,s_track_identity[i],8);
        s_pack_identity=ngs_hash64(s_pack_identity,bytes,sizeof(bytes));
    }
    s_enabled=1; s_volume=volume;
    fprintf(stderr,"[ast] %u mapped music commands, volume %u%%\n",count,volume);
    return 0;
}

int ast_command(uint8_t command) {
    if (!s_enabled) return 0;
    /* The sound driver's NMI handles soft reset before parsing arguments. */
    if (command==3) { ast_reset(); return 0; }
    if (s_pending) {
        if (s_owner==0x0a && command && s_track>=0) {
            /* SNK speed is faster for larger values. Approximate the original
             * attenuation envelope at ~62.5 musical ticks per second. */
            s_fade_total=s_fade_left=UINT64_C(14108444)/command;
            s_fade_gain=s_gain;
        }
        s_pending--; return 0;
    }
    if (s_args[command]) { s_pending=s_args[command]; s_owner=command; return 0; }
    if (command==1 || command==2 || command==4 || command==5 || s_native[command]) stop();
    if (command==0x11) { s_gain=65536; s_fade_left=s_fade_total=0; }
    track_t *t=&s_tracks[command];
    if (!t->number) return 0;
    FILE *f=fopen(t->file,"rb");
    if (!f) {
        fprintf(stderr,"warning: AST unavailable; using native music: %s\n",t->file);
        stop(); return 0;
    }
    stop(); s_file=f; s_track=command;
    fprintf(stderr,"[ast] command $%02x -> CD track %02u (%s)\n",command,t->number,t->loop?"loop":"once");
    return 1;
}

static int sample(uint32_t frame, int channel, int16_t *value) {
    track_t *t=&s_tracks[s_track];
    if (!s_cache_count || frame<s_cache_at || frame-s_cache_at>=s_cache_count) {
        s_cache_at=frame;
        uint32_t n=t->frames-frame;
        if (n>CACHE_FRAMES) n=CACHE_FRAMES;
        if (fseek(s_file,(long)t->offset+(long)frame*4,SEEK_SET) ||
            fread(s_cache,4,n,s_file)!=n) {
            fprintf(stderr,"error: cannot read AST audio: %s\n",t->file);
            stop(); return -1;
        }
        s_cache_count=n;
    }
    *value=(int16_t)le16(s_cache+(frame-s_cache_at)*4+channel*2);
    return 0;
}

void ast_mix(int16_t *out, size_t frames) {
    if (!s_enabled) return;
    for (size_t i=0; i<frames && s_track>=0; i++) {
        track_t *t=&s_tracks[s_track];
        uint64_t limit=(uint64_t)t->frames*PHASE_RATE;
        if (s_phase>=limit) {
            if (t->loop) s_phase%=limit;
            else { stop(); break; }
        }
        uint32_t at=(uint32_t)(s_phase/PHASE_RATE), frac=(uint32_t)(s_phase%PHASE_RATE);
        uint32_t next=at+1;
        if (next==t->frames) next=t->loop?0:at;
        for (int ch=0; ch<2; ch++) {
            int16_t a,b;
            if (sample(at,ch,&a) || sample(next,ch,&b)) return;
            int64_t v=a+((int64_t)(b-a)*frac)/(int64_t)PHASE_RATE;
            v=(v*s_gain*s_volume)/(65536*100);
            v+=out[i*2+ch];
            out[i*2+ch]=(int16_t)(v<-32768?-32768:v>32767?32767:v);
        }
        s_phase+=PHASE_STEP;
        if (s_fade_left) {
            s_fade_left--;
            s_gain=(uint32_t)(((uint64_t)s_fade_gain*s_fade_left)/s_fade_total);
            if (!s_fade_left) stop();
        }
    }
}

void ast_state_cancel(void) { if (s_prepared) fclose(s_prepared); s_prepared=NULL; }
void ast_state(ng_state_t *s) {
    int old_track=s_track;
    int track=ngs_int(s,&s_track);
    unsigned pending=s_pending, owner=s_owner; uint32_t gain=s_gain, fade_gain=s_fade_gain;
    uint64_t phase=s_phase, left=s_fade_left, total=s_fade_total;
    pending=ngs_u32(s,&pending); owner=ngs_u32(s,&owner);
    phase=ngs_u64(s,&phase); left=ngs_u64(s,&left); total=ngs_u64(s,&total);
    gain=ngs_u32(s,&gain); fade_gain=ngs_u32(s,&fade_gain);
    ngs_require(s,track>=-1 && track<256 && pending<=2 && owner<=255 && gain<=65536 && fade_gain<=65536);
    ngs_require(s,left<=total && total<=14108444);
    if (s->error) return;
    if (track>=0) {
        ngs_require(s,s_enabled && s_tracks[track].number && phase<=(uint64_t)s_tracks[track].frames*PHASE_RATE+PHASE_STEP);
        if (s->mode==NGS_CHECK && !s->error && (track!=s_track || !s_file)) {
            ast_state_cancel(); s_prepared=fopen(s_tracks[track].file,"rb");
            ngs_require(s,s_prepared!=NULL);
        }
    } else ngs_require(s,phase==0 && left==0 && total==0);
    if (s->mode==NGS_READ && !s->error) {
        if (track!=old_track || (track>=0 && !s_file)) {
            if (s_file) fclose(s_file);
            s_file=s_prepared; s_prepared=NULL;
        }
        s_track=track; s_phase=phase; s_pending=pending; s_owner=owner;
        s_fade_left=left; s_fade_total=total; s_gain=gain; s_fade_gain=fade_gain;
        s_cache_count=0;
    }
    /* PCM files/metadata and host volume are outside the snapshot. The logical
     * cursor, command parser and envelope are sufficient to reconstruct cache. */
}
