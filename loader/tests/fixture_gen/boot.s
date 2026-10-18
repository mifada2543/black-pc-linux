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
    # Two kernel calls with different argument counts -- one argument and four -- so stdcall argument
    # dropping is checked from game code. They used to be logging stubs, and they kept their ordinals as
    # stubs on purpose for as long as any ordinal was unimplemented; now that the kernel is complete they
    # are real implementations, which is a *stronger* check: the four arguments below reach a function that
    # reads three of them, and the boot thread has to survive that and go on to the timer.
    push 0                             # ordinal 137 takes one argument
    mov eax, dword ptr [THUNK_STUB0]    # slot 31: ordinal 137, KeRemoveQueueDpc
    call eax
    mov [STUB0_RET], eax                # a null DPC is not queued, so this is zero
    push 0                             # ordinal 149 takes four arguments
    push 0
    push 0
    push 0
    mov eax, dword ptr [THUNK_STUB1]    # slot 36: ordinal 149, KeSetTimer
    call eax
    mov [STUB1_RET], eax                # a null timer is not armed, so this is zero

    # A kernel timer and its DPC, which is the only thing in the whole fixture that can reach the DPC
    # worker thread. The DPC routine is game code too: it records the IRQL and the DpcRoutineActive flag it
    # finds at fs:[0x24] and fs:[0x58], plus the thread pointer at fs:[0x28], so the test can check that a
    # DPC really ran at DISPATCH_LEVEL on a thread with its own KPCR -- which is the property that makes
    # this worth having in a fixture rather than only in a unit test.
    mov dword ptr [TIMER_STATE], 1     # "the timer path was taken", so a hang is distinguishable from a skip
    push 1                             # type 1: 10 ms resolution
    push TIMER                         # &timer
    mov eax, dword ptr [THUNK_INITEX]  # slot 26: ordinal 113, KeInitializeTimerEx
    call eax
    push 0                             # KeInitializeDpc(Dpc, Routine, Context): three arguments
    push DPC_ADDR                      # the deferred routine, which is this stub's own boot_dpc
    push DPC_OBJ                       # the DPC object to initialise
    mov eax, dword ptr [THUNK_INITDPC]  # slot 24: ordinal 107, KeInitializeDpc
    call eax
    push DPC_OBJ                       # the fourth word: the DPC to run on expiry
    push 0                             # the third word: the high half of the due time. A LARGE_INTEGER by value
                                        # occupies *two* stack slots, so a three-word call here is read by the
                                        # callee as DueTime = low + (Dpc << 32): an enormous absolute time that
                                        # never comes due, and a DPC pointer read out of the stack.
    push -200000                       # the second word: negative, so relative -- 200000 * 100ns = 20 ms
    push TIMER                         # the first word: &timer
    mov eax, dword ptr [THUNK_SETTIMER]  # slot 36: ordinal 149, the same cell the first call used
    call eax
wait_for_dpc:
    cmp dword ptr [DPC_RAN], 0         # the DPC writes 1 into its own cell when it runs
    je wait_for_dpc
    # A TLS slot, the way game code reaches it: fs:[0xE10 + 4*slot]. The value is written and read back
    # through the same patched addressing, and left where the test can see it. This is the one thing that
    # proves the *thread* state is right: if the displacement resolved to anything but this thread's block,
    # the read-back would not be what was written, and the value would also show up in another thread.
    mov dword ptr fs:[0xE1C], TLS_SLOT_VALUE   # TLS slot 3
    mov eax, fs:[0xE1C]
    mov dword ptr [TLS_SLOT], eax
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

.globl boot_dpc
boot_dpc:                             # KDPC_ROUTINE(Dpc, DeferredContext, SystemArgument1, SystemArgument2)
    mov eax, dword ptr fs:[0x24]      # KPCR.Irql: a DPC runs at DISPATCH_LEVEL, and the game reads it here
    mov dword ptr [DPC_IRQL], eax
    mov eax, dword ptr fs:[0x58]      # KPRCB.DpcRoutineActive
    mov dword ptr [DPC_ACTIVE], eax
    mov eax, dword ptr fs:[0x28]      # KPRCB.CurrentThread: the KTHREAD this DPC is running on
    mov dword ptr [DPC_THREAD], eax
    mov eax, dword ptr [esp+12]      # SystemArgument1, the low word of the 100ns time
    mov dword ptr [DPC_ARGS], eax
    mov dword ptr [DPC_RAN], 1
    ret 16                           # stdcall: the DPC pops its own four arguments, as the console's did
                                    # (the kernel's worker calls it through a stdcall pointer, so a plain
                                    #  ret would leave sixteen bytes of arguments on the stack)

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
