// c4fc F4: switch, which is a jump table in the data segment reached
// through JMPA -- dense, sparse, with a default and without, and with a
// string literal inside the body so the table's data offset is checked
// against c4lc's too.
char *tag;

int dense(int x)
{
  int r;
  r = 0;
  switch (x) { case 1: r = 10; break; case 2: r = 20; break; default: r = 30; }
  return r;
}

int sparse(int x)
{
  int r;
  r = 0;
  switch (x) { case 5: r = 1; break; case 7: r = 2; break; }
  return r;
}

int inside(int x)
{
  switch (x) { case 1: printf("z"); break; case 3: printf("w"); break; }
  return 0;
}

int fallthrough(int x)
{
  int r;
  r = 0;
  switch (x) {
  case 0:
  case 1: r = 1;
  case 2: r = r + 1; break;
  default: r = 9;
  }
  return r;
}

int loopy(int n)
{
  int i, s;
  s = 0;
  for (i = 0; i < n; i++) {
    switch (i) { case 1: continue; case 2: s = s + 100; break; default: s = s + 1; }
    s = s + 1000;
  }
  return s;
}

int main()
{
  tag = "sw";
  return dense(1) + sparse(5) + inside(1) + fallthrough(1) + loopy(4);
}
