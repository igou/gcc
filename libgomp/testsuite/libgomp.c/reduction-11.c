/* { dg-do run { xfail *-*-* } } */

char z[10] = { 0 };

__attribute__((noinline, noclone)) void
foo (int (*x)[3][2], int *y, long w[1][2], int s, int t)
{
  unsigned long long a[9] = {};
  short b[5] = {};
  int i;
  #pragma omp parallel for reduction(+:x[-1:2][:][0:2], z[t + 2:4]) \
			   reduction(*:y[-s:3]) reduction(|:a[s + 3:4]) \
			   reduction(&:w[s + 1:1][t:2]) reduction(max:b[2:])
  for (i = 0; i < 128; i++)
    {
      x[i / 64 - 1][i % 3][(i / 4) & 1] += i;
      if ((i & 15) == 1)
	y[1] *= 3;
      if ((i & 31) == 2)
	y[2] *= 7;
      if ((i & 63) == 3)
	y[3] *= 17;
      z[i / 32 + 2] += (i & 3);
      if (i < 4)
	z[i + 2] += i;
      a[i / 32 + 2] |= 1ULL << (i & 30);
      w[0][i & 1] &= ~(1L << (i / 17 * 3));
      if ((i % 23) > b[2])
	b[2] = i % 23;
      if ((i % 85) > b[3])
	b[3] = i % 85;
      if ((i % 192) > b[4])
	b[4] = i % 192;
    }
  for (i = 0; i < 9; i++)
    if (a[i] != ((i < 6 && i >= 2) ? 0x55555555ULL : 0))
      __builtin_abort ();
  if (b[0] != 0 || b[1] != 0 || b[2] != 22 || b[3] != 84 || b[4] != 127)
    __builtin_abort ();
}

int
main ()
{
  int a[4][3][2] = {};
  static int a2[4][3][2] = {{{ 0, 0 }, { 0, 0 }, { 0, 0 }},
			    {{ 312, 381 }, { 295, 356 }, { 337, 335 }},
			    {{ 1041, 975 }, { 1016, 1085 }, { 935, 1060 }},
			    {{ 0, 0 }, { 0, 0 }, { 0, 0 }}};
  int y[5] = { 0, 1, 1, 1, 0 };
  int y2[5] = { 0, 6561, 2401, 289, 0 };
  char z2[10] = { 0, 0, 48, 49, 50, 51, 0, 0, 0, 0 };
  long w[1][2] = { ~0L, ~0L };
  foo (&a[2], y, w, -1, 0);
  if (__builtin_memcmp (a, a2, sizeof (a))
      || __builtin_memcmp (y, y2, sizeof (y))
      || __builtin_memcmp (z, z2, sizeof (z))
      || w[0][0] != ~0x249249L
      || w[0][1] != ~0x249249L)
    __builtin_abort ();
  return 0;
}
