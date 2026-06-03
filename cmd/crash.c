#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("crash: dereferencing NULL -- expecting SIGSEGV");
    volatile int *p = (volatile int *)0;
    *p = 1;
    puts("crash: BUG -- survived null deref");
    return 1;
}
