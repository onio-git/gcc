/* { dg-do run } */
/* { dg-options "-O2 -funroll-loops -fno-tree-vectorize --param preunroll-factor=2 --param preunroll-max-stmts=20 -fdump-tree-preunroll-details" } */

#define COUNT 67

static unsigned int input[COUNT];
static unsigned int got[COUNT];
static unsigned int expected[COUNT];

static void __attribute__((noinline, noclone))
preunrolled (unsigned int n)
{
  unsigned int i;

  for (i = 0; i < n; ++i)
    got[i] = input[i] * 3 + i;
}

static void __attribute__((noinline, noclone, optimize ("O1")))
reference (unsigned int n)
{
  unsigned int i;

  for (i = 0; i < n; ++i)
    expected[i] = input[i] * 3 + i;
}

int
main (void)
{
  volatile unsigned int n = COUNT;
  unsigned int i;

  for (i = 0; i < COUNT; ++i)
    input[i] = i * 17 + 5;
  preunrolled (n);
  reference (n);
  for (i = 0; i < COUNT; ++i)
    if (got[i] != expected[i])
      __builtin_abort ();
  return 0;
}

/* { dg-final { scan-tree-dump "preunroll: unrolled loop" "preunroll" } } */
