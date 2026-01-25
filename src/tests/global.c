//
// C4CC Test: Global variables having an initial value.
//            We should be able to set global variables to an initial value.
//
//
#include <stdio.h>

enum { SOME_SIZE = 32 };

// Directly setting global
int some_global = 123;
char some_other = 'A';

// Arrays
int some_array[512];        // direct number
int some_sized[SOME_SIZE];  // enum
// int some_error[SOME_SIZE] = { 1 }; // Not supported by c4cc

int main (int argc, char **argv) {
    int local;
    //int local = 1; // Not supported by c4cc
    local = 1;
    printf("some_global = %d\n", some_global);
    printf("some_other  = %c\n", some_other);
    printf("some_array  = %p\n", &some_array);
    printf("some_sized  = %p\n", &some_sized);
    return 0;
}
