// c4fc F7: variadic functions and the constructor/destructor lists.
//
// ... is not a special form -- it is one more parameter, unnamed, and
// the CALL SITE does the work: push everything, push how many were
// extra, call __c4cc_make_va, drop the count and the extras, push what
// it returned. Which is why vsum finds n at bp+3 and not bp+2.
static int *va_stack;
static int va_ptr;
static int va_n, va_v;
int total;

static int *__c4cc_make_va (int count) { va_n = count; return va_stack; }

int vsum(int n, ...) { return n; }
int vmax(int n, int m, ...) { return n + m; }

void __attribute__((constructor)) setup () { total = 1; }
void __attribute__((destructor)) teardown () { total = 0; }

int main()
{
  return vsum(1, 2, 3) + vmax(1, 2, 3, 4, 5) + total;
}
