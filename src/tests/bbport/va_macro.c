#define LOG(fmt, ...) printf("log: " fmt "\n", __VA_ARGS__)
#define LOG2(fmt, args...) printf(fmt, ##args)
int main() { LOG("%d %d", 1, 2); LOG2("gnu\n"); return 0; }
