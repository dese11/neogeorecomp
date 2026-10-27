/*
 * musashi_glue.c — the few Musashi internals the runtime needs, kept in
 * one file so m68kcpu.h's macro namespace (REG_PC, FLAG_Z, ...) stays out
 * of the rest of the runtime.
 */
#include "m68kcpu.h"
#include "musashi_glue.h"

int  mus_stopped(void) { return CPU_STOPPED != 0; }
void mus_clear_stopped(void) { CPU_STOPPED = 0; }
void mus_set_stopped(void) { CPU_STOPPED = STOP_LEVEL_STOP; }
int  mus_base_cycles(unsigned opcode) { return CYC_INSTRUCTION[opcode & 0xFFFF]; }

/* Called only between frames, outside m68k_execute. */
void mus_state(unsigned int v[10], int load) {
#define FIELD(i, x) do { if (load) (x)=v[i]; else v[i]=(x); } while(0)
    FIELD(0,m68ki_cpu.ppc); FIELD(1,m68ki_cpu.ir);
    FIELD(2,m68ki_cpu.instr_mode); FIELD(3,m68ki_cpu.run_mode);
    FIELD(4,m68ki_cpu.reset_cycles); FIELD(5,m68ki_cpu.virq_state);
    FIELD(6,m68ki_cpu.nmi_pending); FIELD(7,m68ki_cpu.int_level);
    FIELD(8,m68ki_cpu.pref_addr); FIELD(9,m68ki_cpu.pref_data);
#undef FIELD
}
