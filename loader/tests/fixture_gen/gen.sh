#!/bin/sh
# Regenerate the boot stub and check it back into tests/xbe_fixture.h.
#
#   cd tests/fixture_gen && ./gen.sh
#
# The script writes the byte array straight into ../xbe_fixture.h, so there is no hand-copy step and no
# chance of shipping the previous run's bytes. Every offset in the header is checked on the next build by
# the static_asserts there, which stand down here only because they would otherwise compare the header
# against the array it is about to replace.
set -e
here=$(dirname "$0")
inc="-I $here/../../src -I $here/.."

# addrs.cpp includes the fixture header, whose static_asserts compare the assembled immediates against
# constants that this run is about to change -- so they stand down here and come back on the next build.
# Assembling takes the DPC's address as an argument, because the stub cannot know where its own deferred
# procedure lands until it has been assembled once.
assemble() {
    g++ -m32 -std=gnu++20 -DBOOTFIX_SKIP_ASM_ASSERTS $inc -o addrs addrs.cpp
    local a
    # Both measured offsets go in: the DPC's, because the stub pushes the routine's address, and the handler's,
    # because the prologue pushes the handler's. Feeding only the first left the second one coming from the
    # header -- the previous run's value -- so the stub pushed an address that was not its handler.
    a=$(./addrs "$@" | while read -r name value; do
            case "$name" in TEXT) continue ;; esac
            printf ' --defsym %s=%s' "$name" "$value"
        done)
    as --32 -o boot.o boot.s $a
    rm -f boot.bin
    objcopy -O binary --only-section=.text boot.o boot.bin
}

label_offset() {
    objdump -d boot.o | awk -v want="<$1>:" '$0 ~ want { print "0x" $1; exit }'
}

# Where the text section starts, straight from addrs.cpp: the shell has to turn an offset into an address, and
# a copy of TEXT_VA here would be one more constant to fall out of step with the header.
text_va=$(./addrs 0 0 | awk '$1 == "TEXT" { print $2 }')
if [ -z "$text_va" ]; then
    echo "FATAL: addrs.cpp printed no TEXT; the generator cannot turn an offset into an address" >&2
    exit 1
fi

# The stub's own address for boot_dpc, which it does not know until it has been assembled, is measured here and
# fed back in. More than one round is needed because the encoding is not the same length either way: `push` of
# a small address is two bytes and of a full one is five, so the code grows when the real address appears and
# every offset below it moves. The loop therefore runs until the measured offset stops changing.
# Both measured offsets are fed back in, because both of them change the encoding: the DPC's because the stub
# pushes the routine's address, the handler's because the prologue pushes the handler's. A run is finished when
# neither of them moves -- testing the DPC alone left the handler one pass behind, and the stub then pushed an
# address that was not its handler, which is a segfault the fixture's own checks could not see.
dpc_va=0
dpc_off=0
handler_off=0
bytes=""
settled=no
for round in 1 2 3 4 5; do
    assemble "$dpc_va" "$handler_off"
    round_dpc=$(label_offset boot_dpc)
    round_handler=$(label_offset boot_handler)
    round_bytes=$(wc -c < boot.bin)
    if [ "$round_dpc" = "$dpc_off" ] && [ "$round_handler" = "$handler_off" ]; then
        bytes=$round_bytes
        settled=yes
        break
    fi
    dpc_off=$round_dpc
    handler_off=$round_handler
    bytes=$round_bytes
    dpc_va=$(printf '0x%08x' $((text_va + round_dpc)))
    echo "  pass $round: boot_dpc at $round_dpc, boot_handler at $round_handler, $round_bytes bytes"
done
if [ "$settled" != yes ]; then
    echo "FATAL: the stub did not settle after five passes: boot_dpc at $round_dpc, boot_handler at $round_handler" >&2
    exit 1
fi
handler_off=$round_handler

# Every symbol the stub uses must be defined. An undefined one becomes an absolute zero at assembly time and
# the stub then reads or calls address zero -- silently, because `as` has no reason to complain about a symbol
# it was never told about. Two names that did not match (boot.s asking for THUNK_INITEX, addrs.cpp printing
# INITEX_THUNK) produced exactly that, and the boot thread died at address zero with nothing in the log to
# say why. The assembler's own list of undefined symbols is the check that cannot be fooled.
undefined=$(nm -u boot.o 2>/dev/null | awk '{ print $NF }' | tr '\n' ' ')
if [ -n "$(echo "$undefined" | tr -d ' ')" ]; then
    echo "FATAL: the stub uses symbols that were never defined: $undefined" >&2
    exit 1
fi

# The flat binary is rebuilt from boot.o every time. An earlier version probed objdump first and only ran
# objcopy when that probe failed -- which it did not, because objdump happily reads the *previous* run's
# boot.bin. The generator then reported success while regenerating from stale bytes, and the fixture's
# immediates silently went back to the values from two changes ago.
rm -f boot.bin
objcopy -O binary --only-section=.text boot.o boot.bin
objdump -d -M intel boot.o > boot.dis

# Every offset the header quotes is measured here and written back, so none of them is ever copied by hand.
# They were copied by hand five times in a row during the timer work and every copy was stale by the time it was
# pasted: the stub moves whenever anything ahead of the DPC changes size, and a stale offset is a stub that
# calls the wrong address while every check still passes.
export HANDLER_OFF="$handler_off" DPC_OFF="$dpc_off"
export HANDLER_VA="$(./addrs "$dpc_va" "$handler_off" | awk '$1 == "HANDLER" { print $2 }')"
export STUB0_VA="$(./addrs "$dpc_va" "$handler_off" | awk '$1 == "THUNK_STUB0" { print $2 }')"
export STUB1_VA="$(./addrs "$dpc_va" "$handler_off" | awk '$1 == "THUNK_STUB1" { print $2 }')"
export RECORD_VA="$(./addrs "$dpc_va" | awk '$1 == "RECORD" { print $2 }')"
export RAISE_EAX_VA="$(./addrs "$dpc_va" | awk '$1 == "OUT_RAISE" { print $2 }')"
for v in HANDLER_OFF DPC_OFF HANDLER_VA RECORD_VA RAISE_EAX_VA STUB0_VA STUB1_VA; do
    if [ -z "$(eval echo \$$v)" ]; then
        echo "FATAL: addrs.cpp printed no $v; the header cannot be updated from a value it never produced" >&2
        exit 1
    fi
done
python3 - <<'PY'
import os, re, subprocess
handler_off = int(os.environ['HANDLER_OFF'], 16)
dpc_off = int(os.environ['DPC_OFF'], 16)
handler_va = int(os.environ['HANDLER_VA'], 16)
record_va = int(os.environ['RECORD_VA'], 16)
raise_eax_va = int(os.environ['RAISE_EAX_VA'], 16)
data = open('boot.bin', 'rb').read()
out = subprocess.run(['objdump', '-D', '-b', 'binary', '-m', 'i386', '-M', 'intel', 'boot.bin'],
                     capture_output=True, text=True).stdout
insns = []
for line in out.splitlines():
    m = re.match(r'\s+([0-9a-f]+):\t([0-9a-f ]+?)\t\s*(\S.*)$', line)
    if m:
        insns.append((int(m.group(1), 16), m.group(3).strip()))
lines, acc = [], b''
for i, (off, txt) in enumerate(insns):
    end = insns[i + 1][0] if i + 1 < len(insns) else len(data)
    span = data[off:end]
    acc += span
    note = '  (boot_handler)' if off == handler_off else ''
    lines.append('    %-44s // %02X  %s%s' % (' '.join('0x%02X,' % b for b in span), off, txt, note))
assert acc == data, 'objdump and the object file disagree'
assert insns[0][0] == 0, 'the entry must start the stub'
assert any(off == handler_off for off, _ in insns), 'boot_handler is not where the header says it is'
# The two immediates the header checks, located by what they push rather than by a remembered offset.
def one_push(va):
    # objdump prints an immediate as few digits as it needs ('push 0x1010ff'), so the numbers are compared as
    # numbers. Matching the text against a zero-padded form finds nothing and looks like a missing push.
    hits = []
    for off, txt in insns:
        m = re.match(r'push\s+(0x[0-9a-fA-F]+)$', txt)
        if m and int(m.group(1), 16) == va:
            hits.append(off)
    assert len(hits) == 1, 'expected exactly one push of 0x%08x, found %r' % (va, hits)
    return hits[0]

prologue_push = one_push(handler_va)
raise_push = one_push(record_va)
store_raise = [i for i, (off, txt) in enumerate(insns)
               if any(int(m.group(1), 16) == raise_eax_va
                      for m in re.finditer(r'0x([0-9a-fA-F]+)', txt))]
assert len(store_raise) == 1, 'the stub stores the raise result once, not %d times' % len(store_raise)
assert insns[store_raise[0] - 1][1].startswith('call'), 'the store of the raise result is not after a call'
raise_return = insns[store_raise[0]][0]
print('%d bytes, %d instructions, boot_handler at 0x%x' % (len(data), len(insns), handler_off))

array = 'constexpr uint8_t kBootCode[] = {\n' + '\n'.join(lines) + '\n};\n'
open('kbootcode.txt', 'w').write(array)

# Into the header, between the markers. The handler length is checked here rather than only in C++: this is
# the one pass where the header's array and its length assert cannot both be true.
header = open('../xbe_fixture.h').read()
start = header.index('constexpr uint8_t kBootCode[] = {')
end = header.index('};', start) + 2
header = header[:start] + array + header[end:]
# The offsets themselves, by name, so the header's own text keeps its comments.
def set_const(text, name, value):
    pattern = re.compile(r'(constexpr uint32_t %s = )0x[0-9A-Fa-f]+(;[^\n]*)' % name)
    assert pattern.search(text), 'the header has no %s constant to update' % name
    return pattern.sub(lambda m: '%s0x%02X%s' % (m.group(1), value, m.group(2)), text, count=1)

header = set_const(header, 'HANDLER_OFF', handler_off)
header = set_const(header, 'BOOT_ENTRY_LEN', handler_off)
header = set_const(header, 'DPC_OFF', dpc_off)
header = set_const(header, 'RAISE_RETURN_OFF', raise_return)
header = set_const(header, 'PROLOGUE_PUSH_HANDLER_OFF', prologue_push)
header = set_const(header, 'RAISE_PUSH_RECORD_OFF', raise_push)


def first_mov_eax(va):
    # The two `mov eax, [thunk slot]` reads, located by the address they load. The second slot is read twice
    # -- once for the null call at the top and once to arm the timer -- so this takes the first, which is the
    # one the header's assert is about.
    for off, txt in insns:
        m = re.match(r'mov\s+eax,ds:(0x[0-9a-fA-F]+)$', txt)
        if m and int(m.group(1), 16) == va:
            return off
    raise AssertionError('the stub never reads 0x%08x' % va)


header = set_const(header, 'THUNK_READ_STUB0_OFF', first_mov_eax(int(os.environ['STUB0_VA'], 16)))
header = set_const(header, 'THUNK_READ_STUB1_OFF', first_mov_eax(int(os.environ['STUB1_VA'], 16)))

# Every instruction that touches %fs, measured. The test patches the image and then walks this list, so a
# form nobody thought to check -- a push of a segment override in an unexpected encoding -- fails the build
# rather than faulting the game at run time.
fs_offsets = [off for off, txt in insns if 'fs:' in txt.lower()]
assert fs_offsets, 'the stub uses no %fs at all, so it is not testing the patch'
fs_array = 'constexpr uint32_t kFsPatchOffsets[] = {\n' + ''.join(
    '    0x%02X,%s\n' % (o, ' ' * max(1, 12 - len('0x%02X,' % o)) + '// %s' %
        next(t for off, t in insns if off == o)) for o in fs_offsets) + '};\n'
marker = 'constexpr uint32_t kFsPatchOffsets[] = {'
if marker in header:
    start = header.index(marker)
    end = header.index('};', start) + 2
    header = header[:start] + fs_array + header[end:]
else:
    anchor = 'constexpr uint32_t PROLOGUE_PUSH_HANDLER_OFF'
    at = header.index(anchor)
    line_end = header.index('\n', at) + 1
    header = header[:line_end] + fs_array + '\n' + header[line_end:]

handler_len = len(data) - handler_off
old = 'static_assert(sizeof kBootCode == BOOT_ENTRY_LEN + 0x27, "the handler follows the entry at 0x7C and runs to 0xA3");'
new = 'static_assert(sizeof kBootCode == BOOT_ENTRY_LEN + 0x%02X, "the handler follows the entry at 0x%02X and runs to 0x%02X");' % (
    handler_len, handler_off, len(data))
if old in header:
    header = header.replace(old, new)
open('../xbe_fixture.h', 'w').write(header)
print('xbe_fixture.h updated: %d bytes of stub, handler 0x%02X..0x%02X' % (len(data), handler_off, len(data)))
PY