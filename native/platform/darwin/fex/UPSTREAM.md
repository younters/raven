# M2 upstream exception research

Checked 2026-10-29, before patch 8. Upstream FEX main at
`d321f49fb5551b7e7ad0e09eee73940bf236e370` (newer than the downstream
FEX-2609.1 base). Sources were fetched directly from GitHub; query responses and
an inspection clone are retained locally in `/tmp/raven-m2-upstream`.

- [Issue 1980](https://github.com/FEX-Emu/FEX/issues/1980), closed: integer
  divide-by-zero does not raise SIGFPE; maintainers describe this as an existing
  known failure because AArch64 divide does not trap. It was closed in favor of
  known-failure lists, not fixed.
- [PR 6047](https://github.com/FEX-Emu/FEX/pull/6047), merged: inlines 128-bit
  division for speed; it does not implement zero/overflow exceptions.
- [PR 3816](https://github.com/FEX-Emu/FEX/pull/3816), merged: corrects signed
  combination of dividend halves; it does not add divide exceptions.
- GitHub issue/PR searches for `divide`, `idiv` and `overflow`, including open
  results, did not reveal a later divide-fault implementation.
- Inspected main `FEXCore/Source/Interface/Core/{OpcodeDispatcher.cpp,JIT/ALUOps.cpp}`
  and `Config/Config.json.in`: no applicable zero/quotient-overflow guard or config
  switch found. AArch64 UDIV/SDIV's nontrapping behavior remains visible.
- Main `Source/Windows/Common/Exception.h` handles fault metadata for SIGILL,
  breakpoint/single-step, GP, SS, OF and PF; it has no #DE case. Its #OF mapping
  is `EXCEPTION_INT_OVERFLOW` (the INTO trap), not DIV/IDIV quotient overflow.
- [Signal design issue 1682](https://github.com/FEX-Emu/FEX/issues/1682) documents
  synchronous state reconstruction and frontend fault-metadata obligations.

Therefore there was no applicable upstream correctness fix to port. Patch 8
adds a Raven-gated architectural divide guard before optimization. Both zero
and unrepresentable quotient emit x86 #DE / STATUS_INTEGER_DIVIDE_BY_ZERO;
#OF remains the separate Windows integer-overflow exception. It reuses the
existing `EmitSynchronousFault`, dispatcher spill stubs, `SynchronousFaultData`
and Windows `HandleGuestException` classifier instead of treating native BRKs
as guest breakpoints. No Wine implementation was copied.
