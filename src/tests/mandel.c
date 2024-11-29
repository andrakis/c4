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

// C4 compatibility
#include <u0.h>

// Removed for C4
//int s(int i);
//int toPrec(float f, int bitsPrecision);

// Default customizable values
enum {
  DEF_WIDTH  = 60,
  DEF_HEIGHT = 40
};

// CHOOSE THE NUMBER OF BITS OF PRECISION - 6 is the most I found useful
// Fixed values for C4 as we can't use floating point.
enum {
  bitsPrecision = 6,
  bits6_4       = 0x100,
  bits6_3point5 = 0xe0,
  bits6_2point5 = 0xa0,
  bits6_2       = 0x80,
  bits6_1       = 0x40
};

// Filled in during main, rendering glyphs
char *chr;

void show_help (char *argv0) {
    printf("Johnlon's Mandelbrot for C4(KE)\n"
           "usage: %s [WxH] [-m] [-c chars]\n", argv0);
    printf("Where:\n"
           "          WxH       Set rendering geometry to W columns by H lines\n"
           "                    (Default: %dx%d)\n", DEF_WIDTH, DEF_HEIGHT);
    printf("          -m        Mono mode, output without color\n"
           "          -c chars  Set fractal rendering glyphs\n"
           "                    (Default: \"%s\")\n", chr);
}

int main(int argc, char** argv)
{
  int log, width, height, X1, X2, Y1, Y2, LIMIT;
  int maxIters, px, py, x0, y0, x, y, i, xSqr, ySqr;
  int notbreak, sum, xt;
  int startTime;
  char **_argv, *arg;
  int _argc;
  int mono;

  log = 0;
  mono = 0;

  width  = DEF_WIDTH;
  height = DEF_HEIGHT;

  // printf("PRECISION=%ld\n", bitsPrecision);

  X1 = bits6_3point5; //toPrec(3.5,bitsPrecision);
  X2 = bits6_2point5; //toPrec(2.5,bitsPrecision) ;
  Y1 = bits6_2; //toPrec(2,bitsPrecision);
  Y2 = bits6_1; //toPrec(1,bitsPrecision) ; // vert pos
  LIMIT = bits6_4; // toPrec(4,bitsPrecision);

  // fractal
  //chr = ".:-=X$#@ ";
  //chr = "12345678 ";
  //chr = "123456789ABCDE ";
  chr = ".,'~=+:;[/<&?oxOX#.";
  //chr = ".,'~=+:;[/<&?oxOX#.!@#$%^&*";

  // Parse arguments
  _argc = argc;
  _argv = argv;
  --_argc; ++_argv; // skip first arg
  while (_argc) {
      arg = *_argv;
      // Flags
      if (*arg == '-') {
          ++arg;
          if (!strcmp(arg, "-h") || !strcmp(arg, "-help")) {
              show_help(argv[0]);
              return 1;
          }
          // -m  Mono mode
          else if (*arg == 'm') mono = 1;
          // -c  Rendering glyphs
          else if (*arg == 'c') {
              // -cXYZ
              if (*(arg + 1)) chr = arg + 1;
              // -c XYZ
              else {
                  --_argc; ++_argv;
                  if (!_argc) {
                      printf("Option -c requires an argument\n\n");
                      show_help(argv[0]);
                      return 1;
                  }
                  chr = *_argv;
              }
          }
      } else if (isnum(*arg)) {
          width = *arg++ - '0';
          height = 0;
          while (*arg && isnum(*arg)) {
              width = width * 10 + (*arg++ - '0');
          }
          if (*arg++ == 'x' && *arg) {
              height = *arg++ - '0';
              while (*arg && isnum(*arg)) {
                  height = height * 10 + (*arg++ - '0');
              }
          }
          if (width <= 0 || height <= 0) {
              printf("Invalid size specification: '%dx%d'.\n", width, height);
              return 1;
          }
          printf("Custom size: %dx%d\n", width, height);
      } else {
          printf("Unrecognised option: '%s'\n", *_argv);
          show_help(argv[0]);
          return 1;
      }
      --_argc; ++_argv;
  }

  maxIters = strlen(chr);
  py = 0;
  startTime = __time();

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

      // C4 doesn't understand \033 and such
      //print("\u001b[48;05;${cl}m  \u001b[0m")
      // printf("\033[48;05;%ldm%02ld\033[0m", i, i);
      // Restored outputting the character instead of the number
      if (mono) printf("%c", chr[i]);
      else printf("%c[48;05;%ldm%c%c[0m", 0x1b, i, chr[i], 0x1b);

      ++log;
      ++px; // px = px + 1;
    }

    printf("\n");
    ++py; // py = py + 1;
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


