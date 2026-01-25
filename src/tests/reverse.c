// C4 Test: reverse an array using pointers
//

#include <stdio.h>
#include <stdlib.h>

void reverse_array (int *arr, int count) {
    int *head, *tail, temp;

    head = arr;
    tail = arr + count - 1;
    while (head < tail) {
        temp = *tail;
        *tail = *head;
        *head = temp;
        ++head;
        --tail;
    }
}

int main () {
    int *arr, count, a, i;

    count = 10;
    a = 1;
    if (!(arr = malloc(sizeof(int) * count))) {
        printf("malloc error\n");
        return 1;
    }

    i = 0;
    while (i < count) {
        arr[i] = a;
        a = a + 1;
        ++i;
    }

    printf("Array: ");
    i = 0; while (i < count) { printf("%d ", arr[i]); ++i; }
    printf("\n");

    reverse_array(arr, count);

    printf("Array: ");
    i = 0; while (i < count) { printf("%d ", arr[i]); ++i; }
    printf("\n");

    free(arr);
    return 0;
}
