/* The ONiO.zero defaults must not enable the optional loop-dispatcher
   inlining limit.  A dispatcher with one caller can become smaller after all
   of its call sites are inlined and the out-of-line copy is removed.  */
/* { dg-do compile } */
/* { dg-options "-O2 -mcpu=onio-zero" } */

volatile int value;

#define KERNEL(N) \
  __attribute__ ((noipa)) int kernel##N (int x) \
  { \
    for (int i = 0; i < 8; ++i) \
      x = x * (N + 2) + value; \
    return x; \
  }

KERNEL (1)
KERNEL (2)
KERNEL (3)
KERNEL (4)

static int
dispatch (int x)
{
  x = kernel1 (x);
  x = kernel2 (x);
  x = kernel3 (x);
  return kernel4 (x);
}

int
wrapper (int x)
{
  return dispatch (x) + dispatch (x + 1);
}

/* All uses of the local dispatcher should be inlined.  */
/* { dg-final { scan-assembler-not "dispatch:" } } */
