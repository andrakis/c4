C4 Lightweight Microkernel, or CALM.
====================================

A kernel that runs inside C4 and provides multitasking and provides a
standard library, but doesn't require altered C4 interpreters like C4m.

STATUS: Non-working, on hold.
REASON: Cannot task switch more than twice successfully. Maybe it's
        because we have no access to the bp register, but modifying
        the value on the stack doesn't seem to work gracefully.

Must be compiled and loaded with boot.c:
  `./c4 boot.c c4lm.c4r`
Or
  `TODO: make run`

boot.c can be compiled with:
  `gcc -E src/load-c4r.c -Iinclude -D__c4cc__=1 -DPURE_C4=1 > boot.c`
Or
  `TODO: make boot.c`

