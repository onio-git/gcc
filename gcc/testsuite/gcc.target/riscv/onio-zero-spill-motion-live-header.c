/* A loop preheader created by loop-spill-motion must have current dataflow
   information.  Stale liveness used to hoist a conditional reload of p[0].a
   even though its hard register carried a value around the enclosing loop.  */
/* { dg-do run } */
/* { dg-options "-O2 -mcpu=onio-zero" } */

typedef struct
{
  int a;
  int b;
} pair;

typedef struct
{
  int a;
  int b;
  int d;
} point;

typedef struct
{
  int f;
  int g;
} count;

typedef struct
{
  count counts[1];
  point points[100];
} glyph;

struct container
{
  glyph value;
} data;

int match, state;
double result;
point *current;
point sentinel;

static int
update (pair *p)
{
  if (p[0].a == match)
    __builtin_abort ();
  int a = p[0].a + p[2].b * (p[2].b - p[0].b);
  int b = (2. + p[4].b - p[2].b) * (p[4].b - p[2].b);
  if (a <= 3 * b)
    {
      p[0] = p[4];
      return 1;
    }
  return 0;
}

static void
walk (struct container *c)
{
  pair p[5];
  point *first = &c->value.points[0];
  if (first->d)
    p[0].a = p[0].b = first->b;
  current = &sentinel;
  result = p[0].b;
  while (c->value.counts[0].g--)
    {
      current = current == &sentinel ? first : current + 1;
      if (current->d)
	switch (state)
	  {
	  case 2:
	    p[4].a = current->a;
	    p[4].b = current->b;
	    state = update (p);
	  }
      else
	switch (state)
	  {
	  case 0:
	    state = 1;
	    break;
	  case 1:
	    p[2].b = current->b;
	    state = 2;
	    break;
	  case 2:
	    if (update (p))
	      state = 1;
	  }
    }
}

int
main (void)
{
  data.value.counts[0] = (count) { 0, 26 };
  data.value.points[0] = (point) { 4, 2, 3 };
  data.value.points[3] = (point) { 2, 126, 3 };
  data.value.points[4] = (point) { 2, 206, 0 };
  data.value.points[6] = (point) { 0, 308, 5 };
  walk (&data);
  return 0;
}
