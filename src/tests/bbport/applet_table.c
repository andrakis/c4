struct applet { const char *name; int (*main)(int, char **); int flags; };
int a_main(int c, char **v) { return 10 + c; }
int b_main(int c, char **v) { return 20 + c; }
static const struct applet applets[] = {
  { "a", a_main, 1 },
  { "b", b_main, 2 },
};
int main() { int i;
  for (i = 0; i < 2; i++) printf("applet %s %d %d\n", applets[i].name, applets[i].main(i, 0), applets[i].flags);
  printf("n %d\n", (int)(sizeof(applets) / sizeof(applets[0]))); return 0; }
