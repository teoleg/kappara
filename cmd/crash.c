#include "ulib.h"
void _start(void) {
    out("crash: dereferencing NULL -- expecting SIGSEGV\r\n");
    volatile int *p = (volatile int *)0;
    *p = 1;
    out("crash: BUG -- survived null deref\r\n");
    sys_exit();
}
