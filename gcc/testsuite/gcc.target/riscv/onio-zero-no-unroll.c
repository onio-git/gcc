/* Verify that ONiO.zero's tuning defaults respect an explicit request not to
   unroll loops.  In particular, -funroll-completely-grow-size must not remain
   enabled as a secondary -funroll-loops default.  Use a volatile struct copy:
   unlike a small scalar loop, this is not independently folded by the early
   complete-peeling pass.  */
/* { dg-do compile } */
/* { dg-options "-O2 -mcpu=onio-zero -fno-unroll-loops -S" } */
/* { dg-skip-if "explicitly re-enables unrolling" { *-*-* } { "-funroll-loops" } } */

typedef struct
{
  unsigned int f0 : 4;
  unsigned int f1 : 11;
  unsigned int f2 : 10;
  unsigned int f3 : 7;
} value_t __attribute__ ((aligned (4)));

static value_t values[] = {
  { .f0 = 7, .f1 = 99, .f3 = 1 },
  { .f0 = 7, .f1 = 251, .f3 = 1 },
  { .f0 = 8, .f1 = 127, .f3 = 5 },
  { .f0 = 5, .f1 = 1, .f3 = 1 },
  { .f0 = 5, .f1 = 1, .f3 = 1 },
  { .f0 = 5, .f1 = 1, .f3 = 1 }
};

void
store_values (volatile value_t *output)
{
  for (unsigned int i = 0; i < 6; ++i)
    output[i + 11] = values[i];
}

/* A loop has one static volatile store; complete unrolling has six.  */
/* { dg-final { scan-assembler-times "\\tsw\\s" 1 } } */
