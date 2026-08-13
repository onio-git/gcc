/* An explicit vector -march combined with the ONiO.zero tune is not a real
   Aldebaran configuration, but it must not leave vector insns without a DFA
   reservation and ICE in the scheduler.  */
/* { dg-do compile } */
/* { dg-options "-O3 -mcpu=onio-zero -march=rv32gcv -mabi=ilp32 -ftree-vectorize -mrvv-max-lmul=dynamic" } */

int value, *table[9], row, column, selected;

static int
scan_table (void)
{
  for (row = 6; row >= 0; --row)
    for (column = 0; column < 2; ++column)
      {
	table[column * 2 + row] = 0;
	selected = value > 1 ? : 0;
	if (selected == 2)
	  return 0;
      }
  return 0;
}

int
main (void)
{
  scan_table ();
  return 0;
}
