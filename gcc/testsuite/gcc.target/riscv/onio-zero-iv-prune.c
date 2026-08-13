/* Limiting unconditional IV candidate-set pruning keeps the initial pointer
   inductions for an unrolled matrix-vector loop.  In particular, it avoids
   three stack temporaries used by the generic parameter value.  */
/* { dg-do compile } */
/* { dg-options "-O2 -mcpu=onio-zero" } */

typedef unsigned int u32;
typedef int matres;
typedef short matdat;

void
matrix_mul_vect (u32 n, matres *c, matdat *a, matdat *b)
{
  for (u32 i = 0; i < n; ++i)
    {
      c[i] = 0;
      for (u32 j = 0; j < n; ++j)
	c[i] += (matres) a[i * n + j] * (matres) b[j];
    }
}

/* { dg-final { scan-assembler-not {sw\t[^\n]*,0\(sp\)} } } */
/* { dg-final { scan-assembler-not {sw\t[^\n]*,4\(sp\)} } } */
/* { dg-final { scan-assembler-not {sw\t[^\n]*,8\(sp\)} } } */
