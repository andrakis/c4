// c4lc L10: escape sequences under -conforming. Without the flag the
// c4cc quirk table applies (\t->8, \r->10, \xHH and \NNN not decoded)
// and this program prints different numbers on purpose - test-c4lc
// pins BOTH, because an opt-in flag that is always on is not opt-in.
// gcc is the oracle for the conforming half.

int main () {
  char *ansi;
  int i;

  ansi = "\033[31mRED\033[0m";
  printf("tab=%d cr=%d nl=%d\n", '\t', '\r', '\n');
  printf("a=%d b=%d f=%d v=%d\n", '\a', '\b', '\f', '\v');
  printf("hexA=%d oct101=%d esc=%d\n", '\x41', '\101', '\033');
  printf("bs=%d qt=%d ap=%d qm=%d\n", '\\', '\"', '\'', '\?');
  // an octal escape stops at three digits: "\0012" is \001 then '2'
  printf("oct3cap=%d then=%d\n", "\0012"[0], "\0012"[1]);
  i = 0;
  while (ansi[i]) { printf("%d ", ansi[i]); ++i; }
  printf("\n");
  return 0;
}
