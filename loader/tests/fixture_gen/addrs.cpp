// The XBE builder behind tests/xbe_fixture.h.
//
// The fixture's machine code is transcribed from objdump output, never typed by hand: the encodings in
// kBootCode came from assembling boot.s and reading the result back (see README.md here). This program
// prints the addresses the assembly needs, so the two cannot drift apart.
#include "xbe_fixture.h"

#include <stdio.h>

// The DPC's address is a property of the assembled code, so it cannot come out of the header: gen.sh
// assembles once to measure it and once more with the number, and checks that the offsets did not move.
// Without an argument the DPC sits at a placeholder that is replaced below, so the first pass still
// assembles and the measurement is real.
#ifndef DPC_VA_PLACEHOLDER
#define DPC_VA_PLACEHOLDER 0
#endif

int main(int argc, char **argv)
{
    using namespace bootfix;
    const uint32_t dpc_va = argc > 1 ? (uint32_t)strtoul(argv[1], nullptr, 0) : (uint32_t)DPC_VA_PLACEHOLDER;
    // boot_handler's offset is measured by gen.sh too, so the handler address printed here is the one the
    // object file actually has. Reading it back out of the header instead would feed the previous run's
    // offset into the next run's stub -- which is how the prologue came to push an address one instruction
    // past the real handler for several runs before anything complained.
    const uint32_t handler_off = argc > 2 ? (uint32_t)strtoul(argv[2], nullptr, 0) : HANDLER_OFF;
    const uint32_t handler_va = TEXT_VA + handler_off;
    printf("TEXT 0x%08x\n"
           "THUNK_STUB0 0x%08x\n"
           "THUNK_STUB1 0x%08x\n"
           "THUNK_RAISE 0x%08x\n"
           "THUNK_UNWIND 0x%08x\n"
           "RECORD 0x%08x\n"
           "OUT_RAISE 0x%08x\n"
           "OUT_UNWIND 0x%08x\n"
           "OUT_HCODE 0x%08x\n"
           "OUT_HEIP 0x%08x\n"
           "OUT_HRAN 0x%08x\n"
           "HANDLER 0x%08x\n"
           "TLS_SLOT 0x%08x\n"
           "TLS_SLOT_VALUE 0x%08x\n"
           "HANDLER_OFF 0x%08x\n"
           "TIMER_STATE 0x%08x\n"
           "DPC_RAN 0x%08x\n"
           "DPC_IRQL 0x%08x\n"
           "DPC_ACTIVE 0x%08x\n"
           "DPC_THREAD 0x%08x\n"
           "DPC_ARGS 0x%08x\n"
           "TIMER 0x%08x\n"
           "THUNK_INITDPC 0x%08x\n"
           "THUNK_INITEX 0x%08x\n"
           "THUNK_SETTIMER 0x%08x\n"
           "DPC_ADDR 0x%08x\n"
           "DPC_OBJ 0x%08x\n"
           "STUB0_RET 0x%08x\n"
           "STUB1_RET 0x%08x\n",
           TEXT_VA, THUNK_VA + SLOT_STUB0 * 4, THUNK_VA + SLOT_STUB1 * 4, THUNK_VA + SLOT_RAISE * 4, THUNK_VA + SLOT_UNWIND * 4, RECORD_VA,
           RAISE_EAX_VA, UNWIND_EAX_VA, HANDLER_CODE_VA, HANDLER_EIP_VA, HANDLER_RAN_VA, handler_va, TLS_SLOT_VA, TLS_SLOT_VALUE, handler_off, TIMER_STATE_VA, DPC_RAN_VA,
           DPC_IRQL_VA, DPC_ACTIVE_VA, DPC_THREAD_VA, DPC_ARGS_VA, TIMER_VA, INITDPC_VA, INITEX_VA, STUB1_VA, dpc_va, DPC_OBJ_VA, STUB0_RET_VA, STUB1_RET_VA);
    return 0;
}