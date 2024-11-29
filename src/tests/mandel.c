// C4 (technically C4KE) port of ascii Mandelbrot
// From: https://github.com/Johnlon/mandelbrot
//
// Original comments follow:
//
//ascii Mandelbrot using 16 bits of fixed point integer maths with a selectable fractional precision in bits.
//
//This is still only 16 bits mathc and allocating more than 6 bits of fractional precision leads to an overflow that adds noise to the plot..
//
//This code frequently casts to int to ensure we're not accidentally benefitting from GCC promotion from int 16 bits to int.
//

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

// C4 compatibility
#define int long long
#pragma GCC diagnostic ignored "-Wformat"
#ifdef C4KE
#include "u0.c"
#else
#define __time() 0
#endif

// Removed for C4
//int s(int i);
//int toPrec(float f, int bitsPrecision);

// C4 lacks a builtin strlen
int our_strlen (char *s) { char *t; t = s; while (*t) ++t; return t - s; }

// CHOOSE THE NUMBER OF BITS OF PRECISION - 6 is the most I found useful
enum { bitsPrecision = 6 };

int main(int argc, char** argv)
{
  int log, width, height, X1, X2, Y1, Y2, LIMIT;
  int maxIters, px, py, x0, y0, x, y, i, xSqr, ySqr;
  int notbreak, sum, xt;
  int startTime;
  char *chr;

  log = 0;

  width = 60;
  height = 40;

  // printf("PRECISION=%ld\n", bitsPrecision);

  X1 = 0xe0; //toPrec(3.5,bitsPrecision);
  X2 = 0xa0; //toPrec(2.5,bitsPrecision) ;
  Y1 = 0x80; //toPrec(2,bitsPrecision);
  Y2 = 0x40; //toPrec(1,bitsPrecision) ; // vert pos
  LIMIT = 0x100; // toPrec(4,bitsPrecision);

  // fractal
  //chr = ".:-=X$#@ ";
  //chr = "12345678 ";
  //chr = "123456789ABCDE ";
  //chr = ".,'~=+:;[/<&?oxOX#.";
  chr = ".,'~=+:;[/<&?oxOX#.!@#$%^&*";
  maxIters = our_strlen(chr);

  startTime = __time();

  py = 0;
  while (py < height) {
    px = 0;
    while (px < width) {
      x0 = ((px*X1) / width) - X2;
      y0 = ((py*Y1) / height) - Y2;

      x = 0;
      y = 0;
      i = 0;

      notbreak = 1;
      while (notbreak && i < maxIters) {
        xSqr = (x * x) >> bitsPrecision;
        ySqr = (y * y) >> bitsPrecision;

        // Breakout if sum is > the limit OR breakout also if sum is negative which indicates overflow of the addition has occurred
        // The overflow check is only needed for precisions of over 6 bits because for 7 and above the sums come out overflowed and negative therefore we always run to maxIters and we see nothing.
        // By including the overflow break out we can see the fractal again though with noise.
        sum = (xSqr + ySqr);
        if (sum > LIMIT) {
          notbreak = 0;
        } else {
          xt = xSqr - ySqr + x0;

// if (log == 26) {
//   printf("\n");
//   printf("i    %4x\n", i);
//   printf("diff %4x\n", (xSqr - ySqr)&0xffff);
//   printf("x %4x\n", x & 0xffff);
//   printf("y %4x\n", y & 0xffff);
//   printf("m %4x\n", (x*y) & 0xffff);
//   printf("m6 %4x\n", (s(x*y)>>6) & 0xffff);
//   printf("m1 %4x\n", (s(s(x*y)>>6) <<1) & 0xffff);
// }

          y = (((x * y) >> bitsPrecision) << 1) + y0;
          x = xt;

          ++i; //i = i + 1;
        }
      }
      --i; //i = i - 1;

// if (log == 26) {
//   exit(1);
// }

      //printf("%c", chr[i]);
      //print("\u001b[48;05;${cl}m  \u001b[0m")
      // C4 doesn't understand \033 and such
      // printf("\033[48;05;%ldm%02ld\033[0m", i, i);
      // Restored outputting the character instead of the number
      printf("%c[48;05;%ldm%c%c[0m", 0x1b, i, chr[i], 0x1b);

      ++log;
      px = px + 1;
    }

    printf("\n");
    py = py + 1;
  }

  printf("Mandelbrot rendered in %ldms\n", __time() - startTime);

  return 0;
}

// convert decimal value to a fixed point value in the given precision
// removed and replaced with constants
//int toPrec(float f, int bitsPrecision) {
//  int whole = ((int)floor(f) << (bitsPrecision));
//  int part = (f-floor(f))*(pow(2,bitsPrecision));
//  int ret = whole + part;
//  printf("float %f with precision %ld = 0x%lx\n", f, bitsPrecision, ret);
//  return ret;
//}

// not so convenient on c4
// convenient casting
//int s(int i) {
//  return i;
//}


