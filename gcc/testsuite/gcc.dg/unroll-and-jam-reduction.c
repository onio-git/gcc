/* { dg-do run } */
/* { dg-options "-O3 -floop-unroll-and-jam -fno-tree-loop-distribute-patterns -fno-tree-vectorize --param unroll-jam-min-percent=0 --param unroll-jam-max-unroll=4 --param unroll-jam-allow-reductions=1 -fdump-tree-unrolljam-details" } */

#define ROWS 8
#define COLS 37

static unsigned int a[ROWS][COLS];
static unsigned int b[COLS];
static unsigned int got[ROWS];
static unsigned int expected[ROWS];
static unsigned int side[ROWS];
static unsigned int side_expected[ROWS];

static void __attribute__((noinline, noclone))
jammed (unsigned int rows)
{
  unsigned int i, j;

  if (rows == 0)
    return;
  for (i = 0; i < rows; ++i)
    {
      got[i] = 0;
      j = 0;
      do
	{
	  got[i] += a[i][j] * b[j];
	  ++j;
	}
      while (j < COLS);
    }
}

static void __attribute__((noinline, noclone, optimize ("O1")))
reference (unsigned int rows)
{
  unsigned int i, j;

  for (i = 0; i < rows; ++i)
    {
      unsigned int sum = 0;

      for (j = 0; j < COLS; ++j)
	sum += a[i][j] * b[j];
      expected[i] = sum;
    }
}

/* This accumulator is shared across outer iterations and must not be split
   into independent accumulators by unroll-and-jam.  */
static unsigned int __attribute__((noinline, noclone))
shared (unsigned int rows, unsigned int sum)
{
  unsigned int i, j;

  if (rows == 0)
    return sum;
  for (i = 0; i < rows; ++i)
    {
      j = 0;
      do
	{
	  sum += a[i][j] * b[j];
	  ++j;
	}
      while (j < COLS);
    }
  return sum;
}

static unsigned int
__attribute__((noinline, noclone, optimize ("O1")))
shared_reference (unsigned int rows, unsigned int sum)
{
  unsigned int i, j;

  for (i = 0; i < rows; ++i)
    for (j = 0; j < COLS; ++j)
      sum += a[i][j] * b[j];
  return sum;
}

/* An unrelated outer-loop store remains a fusion barrier even when the inner
   loop has a valid reduction.  */
static void __attribute__((noinline, noclone))
with_side_store (unsigned int rows)
{
  unsigned int i, j;

  if (rows == 0)
    return;
  for (i = 0; i < rows; ++i)
    {
      got[i] = 0;
      j = 0;
      do
	{
	  got[i] += a[i][j] * b[j];
	  ++j;
	}
      while (j < COLS);
      side[i] = i * 7 + 11;
    }
}

static void __attribute__((noinline, noclone, optimize ("O1")))
with_side_store_reference (unsigned int rows)
{
  unsigned int i, j;

  for (i = 0; i < rows; ++i)
    {
      unsigned int sum = 0;

      for (j = 0; j < COLS; ++j)
	sum += a[i][j] * b[j];
      expected[i] = sum;
      side_expected[i] = i * 7 + 11;
    }
}

int __attribute__((optimize ("O1")))
main (void)
{
  unsigned int i;
  volatile unsigned int rows = ROWS;

  /* Keep setup one-dimensional so the dump check below can only match one
     of the three loop nests under test.  */
  for (i = 0; i < ROWS * COLS; ++i)
    ((unsigned int *) a)[i] = (i * 17 + 3) & 255;
  for (i = 0; i < COLS; ++i)
    b[i] = (i * 13 + 5) & 63;

  jammed (rows);
  reference (rows);
  for (i = 0; i < ROWS; ++i)
    if (got[i] != expected[i])
      __builtin_abort ();
  if (shared (rows, 17) != shared_reference (rows, 17))
    __builtin_abort ();
  with_side_store (rows);
  with_side_store_reference (rows);
  for (i = 0; i < ROWS; ++i)
    if (got[i] != expected[i] || side[i] != side_expected[i])
      __builtin_abort ();
  return 0;
}

/* { dg-final { scan-tree-dump-times "applying unroll and jam" 1 "unrolljam" } } */
