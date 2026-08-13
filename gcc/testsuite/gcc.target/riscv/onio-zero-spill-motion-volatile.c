/* { dg-do compile } */
/* { dg-require-effective-target rv32 } */
/* { dg-options "-O2 -mcpu=onio-zero -fno-unroll-loops -fno-unroll-all-loops -fdump-rtl-spillmotion-details" } */

/* Allocator spill slots and user stack objects can both be addressed relative
   to sp or fp.  Volatile user objects must never be treated as spills.  */

__attribute__ ((noinline, noclone)) int
load_volatile (int n)
{
  volatile int value = 7;
  int sum = 0;

  for (int i = 0; i < n; ++i)
    sum += value;
  return sum;
}

__attribute__ ((noinline, noclone)) int
store_volatile (int n)
{
  volatile int value = 0;

  for (int i = 0; i < n; ++i)
    value = n;
  return value;
}

/* { dg-final { scan-rtl-dump-not "invariant reload" "spillmotion" } } */
/* { dg-final { scan-rtl-dump-not "exit-only store" "spillmotion" } } */
