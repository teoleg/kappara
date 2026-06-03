#include <stdio.h>

int main(void)
{
    puts("crash: dereferencing NULL -- expecting SIGSEGV");
    volatile int *p = (volatile int *)0;
    *p = 1;
    puts("crash: BUG -- survived null deref");
    return 1;
}
