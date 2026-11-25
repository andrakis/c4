enum colour { RED, GREEN = 5, BLUE };
typedef enum colour colour_t;
int main() { colour_t c; c = BLUE; printf("te %d\n", c); return 0; }
