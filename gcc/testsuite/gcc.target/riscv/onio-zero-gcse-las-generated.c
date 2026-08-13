/* Load-after-store GCSE is useful for compiler-generated loop-store-motion
   temporaries, but applying it to user pointer stores can increase register
   pressure sharply.  Verify that the selective ONiO.zero default still
   eliminates the generated stores and reloads in this state-machine loop.  */
/* { dg-do compile } */
/* { dg-options "-O2 -mcpu=onio-zero --param max-inline-insns-auto=160 -fdump-rtl-pre-details" } */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef short s16;

enum state
{
  START,
  INVALID,
  SIGN1,
  SIGN2,
  INTEGER,
  FLOAT,
  EXPONENT,
  SCIENTIFIC,
  NUM_STATES
};

extern u16 fold (u32, u16);

static enum state transition (u8 **, u32 *);

u16
state_bench (u32 size, u8 *data, s16 seed1, s16 seed2, s16 step, u16 crc)
{
  u32 final[NUM_STATES];
  u32 counts[NUM_STATES];
  u8 *p = data;

  for (u32 i = 0; i < NUM_STATES; ++i)
    final[i] = counts[i] = 0;

  while (*p)
    final[transition (&p, counts)]++;

  p = data;
  while (p < data + size)
    {
      if (*p != ',')
	*p ^= (u8) seed1;
      p += step;
    }

  p = data;
  while (*p)
    final[transition (&p, counts)]++;

  p = data;
  while (p < data + size)
    {
      if (*p != ',')
	*p ^= (u8) seed2;
      p += step;
    }

  for (u32 i = 0; i < NUM_STATES; ++i)
    {
      crc = fold (final[i], crc);
      crc = fold (counts[i], crc);
    }
  return crc;
}

static inline u8
is_digit (u8 c)
{
  return c >= '0' && c <= '9';
}

static enum state
transition (u8 **input, u32 *counts)
{
  u8 *p = *input;
  enum state state = START;

  for (; *p && state != INVALID; ++p)
    {
      u8 c = *p;
      if (c == ',')
	{
	  ++p;
	  break;
	}

      switch (state)
	{
	case START:
	  if (is_digit (c))
	    state = INTEGER;
	  else if (c == '+' || c == '-')
	    state = SIGN1;
	  else if (c == '.')
	    state = FLOAT;
	  else
	    {
	      state = INVALID;
	      counts[INVALID]++;
	    }
	  counts[START]++;
	  break;

	case SIGN1:
	  if (is_digit (c))
	    {
	      state = INTEGER;
	      counts[SIGN1]++;
	    }
	  else if (c == '.')
	    {
	      state = FLOAT;
	      counts[SIGN1]++;
	    }
	  else
	    {
	      state = INVALID;
	      counts[SIGN1]++;
	    }
	  break;

	case INTEGER:
	  if (c == '.')
	    {
	      state = FLOAT;
	      counts[INTEGER]++;
	    }
	  else if (!is_digit (c))
	    {
	      state = INVALID;
	      counts[INTEGER]++;
	    }
	  break;

	case FLOAT:
	  if (c == 'E' || c == 'e')
	    {
	      state = SIGN2;
	      counts[FLOAT]++;
	    }
	  else if (!is_digit (c))
	    {
	      state = INVALID;
	      counts[FLOAT]++;
	    }
	  break;

	case SIGN2:
	  if (c == '+' || c == '-')
	    {
	      state = EXPONENT;
	      counts[SIGN2]++;
	    }
	  else
	    {
	      state = INVALID;
	      counts[SIGN2]++;
	    }
	  break;

	case EXPONENT:
	  if (is_digit (c))
	    {
	      state = SCIENTIFIC;
	      counts[EXPONENT]++;
	    }
	  else
	    {
	      state = INVALID;
	      counts[EXPONENT]++;
	    }
	  break;

	case SCIENTIFIC:
	  if (!is_digit (c))
	    {
	      state = INVALID;
	      counts[INVALID]++;
	    }
	  break;

	default:
	  break;
	}
    }

  *input = p;
  return state;
}

/* Seven counter slots are copied into reaching registers at each of the two
   store-motion exits.  */
/* { dg-final { scan-rtl-dump-times "store updated with reaching reg" 14 "pre" } } */
