// The XBE builder behind tests/xbe_fixture.h.
//
// The fixture's machine code is transcribed from objdump output, never typed by hand: the encodings in
// kBootCode came from assembling boot.s and reading the result back (see README.md here). This program
// prints the addresses the assembly needs, so the two cannot drift apart.
#include "xbe_fixture.h"

#include <stdio.h>

int main()
{
    using namespace bootfix;
    printf("THUNK0 0x%08x\n"
           "THUNK1 0x%08x\n"
           "THUNK_RAISE 0x%08x\n"
           "THUNK_UNWIND 0x%08x\n"
           "RECORD 0x%08x\n"
           "OUT_RAISE 0x%08x\n"
           "OUT_UNWIND 0x%08x\n"
           "OUT_HCODE 0x%08x\n"
           "OUT_HEIP 0x%08x\n"
           "OUT_HRAN 0x%08x\n"
           "HANDLER 0x%08x\n",
           THUNK_VA, THUNK_VA + 4, THUNK_VA + SLOT_RAISE * 4, THUNK_VA + SLOT_UNWIND * 4, RECORD_VA,
           RAISE_EAX_VA, UNWIND_EAX_VA, HANDLER_CODE_VA, HANDLER_EIP_VA, HANDLER_RAN_VA, HANDLER_VA);
    return 0;
}