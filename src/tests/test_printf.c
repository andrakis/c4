#include <stdio.h>
#include <stdlib.h>

#define int long long

int main (int argc, char **argv) {
    int an_int, *an_int_ptr;
    char a_char, *a_char_ptr, *a_string;

    an_int = 65;
    a_char = 65;
    an_int_ptr = &an_int;
    a_string = "Testing";
    a_char_ptr = a_string;

    printf("An int: %lld\n", an_int);
    printf("a char: %c\n", a_char);
    printf("an int ptr: %p\n", an_int_ptr);
    printf("a string: %s\n", a_string);
    printf("a char pointer: %.*s\n", 5, a_char_ptr);

    printf("All together now:\n"
           "An int: %lld\n"
           "a char: %c\n"
           "an int ptr: %p\n"
           "a string: %s\n"
           "a char pointer: %.*s\n",
           an_int, a_char, an_int_ptr, a_string,
           5, a_char_ptr);


    exit(0); // until c4.js bug is fixed
    return 0;
}
