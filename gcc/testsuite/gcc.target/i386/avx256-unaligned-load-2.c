/* { dg-do compile { target { ! ia32 } } } */
/* { dg-options "-O3 -dp -mavx -mavx256-split-unaligned-load" } */

void
avx_test (char **cp, char **ep)
{
  int i;
  char **ap = __builtin_assume_aligned (ep, 32);
  for (i = 128; i > 0; i--)
    *ap++ = *cp++;
}

/* { dg-final { scan-assembler-not "avx_loaddqu256" } } */
/* { dg-final { scan-assembler "sse2_loaddqu" } } */
/* { dg-final { scan-assembler "vinsert.128" } } */
