int g[3][4];
int main() { int i; int j; int l[2][3];
  for (i = 0; i < 3; i++) for (j = 0; j < 4; j++) g[i][j] = i * 10 + j;
  for (i = 0; i < 2; i++) for (j = 0; j < 3; j++) l[i][j] = i + j;
  printf("2d %d %d %d\n", g[2][3], l[1][2], (int)sizeof(g)); return 0; }
