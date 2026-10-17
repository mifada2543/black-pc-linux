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
g++ -m32 -std=gnu++20 -DBOOTFIX_SKIP_ASM_ASSERTS $inc -o addrs addrs.cpp

args=""
./addrs | while read -r name value; do
    printf ' --defsym %s=%s' "$name" "$value"
done > /tmp/defsyms.txt
args=$(cat /tmp/defsyms.txt)
# Where boot_handler lands, from the constants in the header rather than typed in here: a hardcoded offset
# in this script went stale the first time the stub grew and left a comment pointing at the wrong bytes.
handler_off=$(./addrs | awk '$1 == "HANDLER_OFF" { print $2 }')

as --32 -o boot.o boot.s $args
# The flat binary is rebuilt from boot.o every time. An earlier version probed objdump first and only ran
# objcopy when that probe failed -- which it did not, because objdump happily reads the *previous* run's
# boot.bin. The generator then reported success while regenerating from stale bytes, and the fixture's
# immediates silently went back to the values from two changes ago.
rm -f boot.bin
objcopy -O binary --only-section=.text boot.o boot.bin
objdump -d -M intel boot.o > boot.dis

HANDLER_OFF="$handler_off" python3 - <<'PY'
import os, re, subprocess
handler_off = int(os.environ['HANDLER_OFF'], 16)
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
print('%d bytes, %d instructions, boot_handler at 0x%x' % (len(data), len(insns), handler_off))

array = 'constexpr uint8_t kBootCode[] = {\n' + '\n'.join(lines) + '\n};\n'
open('kbootcode.txt', 'w').write(array)

# Into the header, between the markers. The handler length is checked here rather than only in C++: this is
# the one pass where the header's array and its length assert cannot both be true.
header = open('../xbe_fixture.h').read()
start = header.index('constexpr uint8_t kBootCode[] = {')
end = header.index('};', start) + 2
header = header[:start] + array + header[end:]
handler_len = len(data) - handler_off
old = 'static_assert(sizeof kBootCode == BOOT_ENTRY_LEN + 0x27, "the handler follows the entry at 0x7C and runs to 0xA3");'
new = 'static_assert(sizeof kBootCode == BOOT_ENTRY_LEN + 0x%02X, "the handler follows the entry at 0x%02X and runs to 0x%02X");' % (
    handler_len, handler_off, len(data))
if old in header:
    header = header.replace(old, new)
open('../xbe_fixture.h', 'w').write(header)
print('xbe_fixture.h updated: %d bytes of stub, handler 0x%02X..0x%02X' % (len(data), handler_off, len(data)))
PY