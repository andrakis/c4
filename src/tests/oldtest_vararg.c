#include <stdarg.h>
#include <stdio.h>

int
add_em_up (int count,...)
{
  va_list ap;
  int i, sum, x;
  int *y;

  va_start (ap, count);         /* Initialize the argument list. */

  printf("ap is at 0x%lx, count is at 0x%lx, difference 0x%lx\n", &ap, &count, (void *)&count - (void*)&ap);
  printf("*ap = %ld\n", *((int *)ap));
  printf("add_em_up: I think the count is %d\n", count);

  sum = 0;
  // for (i = 0; i < count; i++)
  i = 0;
  while (i < count) {
    x = va_arg (ap, int);    /* Get the next argument value. */
    printf("iteration %ld got argument %ld\n", i, x);
    sum = sum + x;
    ++i;
  }

  va_end (ap);                  /* Clean up. */
  return sum;
}

int main ()
{
  /* This call prints 16. */
  printf ("%d\n", add_em_up (3, 5, 5, 6));

  /* This call prints 56. */
  printf ("%d\n", add_em_up (10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11));

  return 0;
}
