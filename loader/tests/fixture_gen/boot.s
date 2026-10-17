# The boot stub for the synthetic XBE, in Intel syntax so objdump output is the source of truth.
#
# The addresses are not written here: gen.sh passes them in with `as --defsym`, taken from the fixture
# header itself (via addrs.cpp). An earlier version of this file had the addresses typed in, one of
# them a digit short, and the stub happily read the section table and called it -- which is the kind of
# failure a fixture must not be able to have.
.intel_syntax noprefix
.code32
.text
.globl boot_entry
boot_entry:
    mov eax, dword ptr [THUNK0]        # slot 0: ordinal 1
    call eax
    push 0                             # ordinal 2 takes four arguments
    push 0
    push 0
    push 0
    mov eax, dword ptr [THUNK1]        # slot 1: ordinal 2
    call eax
    push HANDLER                       # the handler below
    push dword ptr fs:[0]              # the old chain head -- MSVC pushes the handler first, so the
                                        # record is { prev, handler } (see seh.h)
    mov dword ptr fs:[0], esp          # install the frame
    push RECORD                        # &record
    mov eax, dword ptr [THUNK_RAISE]   # slot 95: ordinal 302, RtlRaiseException
    call eax
    mov dword ptr [OUT_RAISE], eax     # EAX after the raise
    mov eax, dword ptr fs:[0]          # the frame just installed
    push 0x0000DEAD                    # return value   \ stdcall arguments are pushed in reverse
    push 0                             # exception record  } order, so the callee reads target frame,
    push 0                             # target ip         } ip, record, return value
    push eax                           # target frame    /
    mov eax, dword ptr [THUNK_UNWIND]  # slot 99: ordinal 312, RtlUnwind
    call eax
    mov dword ptr [OUT_UNWIND], eax    # EAX after the unwind
    mov ecx, dword ptr [esp]           # the old head: esp is the record and the link is at +0
    mov dword ptr fs:[0], ecx          # restore the chain
    add esp, 8
    ret

.globl boot_handler
boot_handler:                          # long handler(record, establisher, context, dispatcher)
    mov eax, dword ptr [esp+4]         # record
    mov eax, dword ptr [eax]           # ExceptionCode
    mov dword ptr [OUT_HCODE], eax
    mov eax, dword ptr [esp+12]        # context -- [esp+8] is the establisher frame
    mov eax, dword ptr [eax+0xB8]     # Context->Eip
    mov dword ptr [OUT_HEIP], eax
    mov dword ptr [OUT_HRAN], 0x0000C0DE   # "the handler ran"
    xor eax, eax                       # ExceptionContinueExecution
    ret
