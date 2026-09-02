/* Verify that ext-dce does not treat the destination of a memory-load SET
   as a use when computing bit liveness around a loop.  */
/* { dg-do compile } */
/* { dg-options "-O2 -march=rv32imc_zbb -mabi=ilp32" } */

void
test_half_loop (signed short *p, unsigned int n)
{
  for (unsigned int i = 0; i < n; ++i)
    {
      int value = p[i];
      p[i] = (value & 0xff00) | ((unsigned short) value >> 8);
    }
}

/* { dg-final { scan-assembler {\mlhu\M} } } */
/* { dg-final { scan-assembler-not {\mlh\M} } } */
/* { dg-final { scan-assembler-not {\mzext\.h\M} } } */
