# Fixture generator

`tests/xbe_fixture.h` builds a synthetic XBE whose `.text` is real x86 code: a boot stub that calls
through the kernel import table, installs an MSVC-shaped SEH frame, raises an exception and unwinds.
The byte array in the header is transcribed from this directory's output, never typed by hand — the same
rule the fs→gs patcher fixtures follow, for the same reason: a fixture whose encoding is one byte wrong
will happily agree with a loader that is also one byte wrong.

## Regenerating

```sh
cd tests/fixture_gen
./gen.sh                                   # assemble + transcribe; prints a byte count
diff <(sed -n '/^constexpr uint8_t kBootCode/,/^};/p' ../xbe_fixture.h) kbootcode.txt
```

`gen.sh` passes the addresses to the assembler from the fixture header itself (`addrs.cpp`), so the
assembly and the C++ constants cannot drift apart. The `static_assert`s in the header check that the
committed bytes still match `THUNK_VA`, `HANDLER_VA` and `RECORD_VA`.

## Why it exists

This fixture found two real bugs in the port that no unit test could have caught, because the unit
tests build SEH frames with the loader's own (wrong) assumptions:

- **The registration record layout.** MSVC pushes the handler *first* and the old chain head second, so
  the record at `fs:[0]` is `{ prev, handler }` — link at +0. The port had it the other way round,
  which meant the dispatcher called the previous chain head as if it were a handler.
- **Ordinal 302's calling convention.** `RtlRaiseException` is `__stdcall`, so the callee pops the
  argument. A cdecl binding left the game's stack four bytes short after every raise; the symptom was
  the boot entry returning into the address of its own frame handler.

Both were invisible to a test that constructs records and call sites by hand, and both would have been
fatal on the real image. That is the argument for fixtures that run *game-shaped* code.