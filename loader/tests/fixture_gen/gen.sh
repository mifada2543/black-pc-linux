#!/bin/sh
# Regenerate the boot stub and check it back into tests/xbe_fixture.h.
#
#   cd tests/fixture_gen && ./gen.sh && diff <(sed -n '/^constexpr uint8_t kBootCode/,/^};/p' ../xbe_fixture.h) kbootcode.txt
#
# The last command prints nothing when the header's byte array is exactly what the assembler produced.
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

as --32 -o boot.o boot.s $args
# The flat binary is rebuilt from boot.o every time. An earlier version probed objdump first and only ran
# objcopy when that probe failed -- which it did not, because objdump happily reads the *previous* run's
# boot.bin. The generator then reported success while regenerating from stale bytes, and the fixture's
# immediates silently went back to the values from two changes ago.
rm -f boot.bin
objcopy -O binary --only-section=.text boot.o boot.bin
objdump -d -M intel boot.o > boot.dis

python3 - <<'PY'
import re, subprocess
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
    note = '  (boot_handler)' if off == 0x64 else ''
    lines.append('    %-44s // %02X  %s%s' % (' '.join('0x%02X,' % b for b in span), off, txt, note))
assert acc == data, 'objdump and the object file disagree'
open('kbootcode.txt', 'w').write('constexpr uint8_t kBootCode[] = {\n' + '\n'.join(lines) + '\n};\n')
print('%d bytes, %d instructions -- copy kbootcode.txt over the array in ../xbe_fixture.h' % (len(data), len(insns)))
PY