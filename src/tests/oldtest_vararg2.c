#include <stdarg.h>
#include <stdio.h>

int our_printf (int fd, char *fmt, ...) {
	va_list ap;
	int bytes;
	char *c, *s;
	int _c, *_x;
	char *_y;

	printf("fd: %ld\n", fd);
	printf("fmt: %s\n", fmt);
	return 0;
	//va_start(ap, fmt);
	_y = &fmt;

	printf("Print to fd %ld\n", fd);
	printf("Possible _y: %ld\n", _y / sizeof(int));
	printf("Possible arg count: %ld\n", &ap - &fmt);
	printf("I think fmt is at 0x%lx: %s\n", fmt, fmt);

	return 0;
	printf("fmt addr0: 0x%lx\n", &fmt);
	fmt = &fmt + (4 * sizeof(int));
	printf("fmt addr1: 0x%lx\n", fmt);
	//printf("fmt val0: 0x%lx\n", *(((int *)&fmt) + (c * sizeof(int))));
	//fmt = &fmt + (sizeof(int) * (c + 6));
	//printf("fmt addr1: 0x%lx\n", fmt);
	printf("fmt: %s\n", fmt);

	return 0;

	c = fmt;
	while (*c) {
		if (*c == '%') {
			++c;
			if (*c == 's') {
				s = va_arg(ap, char *);
				bytes = bytes + puts(s);
			} else {
				printf("Invalid format argument: %c\n", c);
			}
		} else {
			putchar(*c);
		}
		++c;
	}

	return bytes;
}

int main ()
{
	char *a, *b;
	int   fd;
	a = "Hello %s";
	b = "world";
	printf("Calling our_printf with arg '%s' (0x%lx), '%s' (0x%lx)\n", a, &a, b, &b);
	fd = 1;
	our_printf(fd, a, b);
	our_printf(fd, a, b, "foo", "bar");
	printf("\n");

	return 0;
}

