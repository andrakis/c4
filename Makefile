NAME        := c4
MULTI       := c4_multiload

CC          := cc
CCOR1k      := /opt/or1k-linux-musl/bin/or1k-musl-linux-gcc
CFLAGS      := -g -O2 -Wno-format
DEPFLAGS     =

LDFLAGS     :=
LDLIBS      :=

VPATH       := ./
SRCS        := c4.c
SRCS_MULTI  := c4_multiload.c
OBJS        := $(SRCS:%.c=%.o)
OBJS_MULTI  := $(SRCS_MULTI:%.c=%.o)
DEPS        := $(SRCS:%.c=%.d)
DEPS_MULTI  := $(SRCS_MULTI:%.c=%.d)

.PHONY: all clean test

$(NAME): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@ $(LDLIBS)

$(MULTI): $(OBJS_MULTI)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS_MULTI) -o $@ $(LDLIBS)

%.o: %.c %.d
	$(CC) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c $< -o $@

all: $(NAME) $(MULTI)

clean:
	$(RM) $(OBJS) $(DEPS) $(OBJS_MULTI) $(DEPS_MULTI) $(NAME) $(MULTI)

test:
	./$(NAME) $(SRCS_MULTI) classes.c classes_test.c

or1k:
	$(RM) c4-or1k.o c4_multiload-or1k.o c4 c4-or1k-new.tgz
	$(CCOR1k) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c c4.c -o c4-or1k.o
	$(CCOR1k) $(CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c c4_multiload.c -o c4_multiload-or1k.o
	$(CCOR1k) $(CFLAGS) $(LDFLAGS) c4-or1k.o $(LDLIBS) -o c4
	$(CCOR1k) $(CFLAGS) $(LDFLAGS) c4_multiload-or1k.o $(LDLIBS) -o c4_multiload
	tar cjf c4-or1k-new.tgz c4 *.c c4_multiload stdlib/*.c stdlib/*.h