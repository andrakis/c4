# c4fp: IEEE floating point in software

`softfp.c` is IEEE 754 binary32 (`float`) and binary64 (`double`)
arithmetic, written in the C that c4lc compiles. Each operation is a
function on bit patterns:
- a float is an `unsigned int`;
- a double is an `unsigned long long`, one word on a 64-bit machine and two on a 32-bit one.

So the same source serves both word sizes, and gcc as well.

It is what c4lc's `float` and `double` compile to (docs/c4-float.md):
- `__c4f_add`/`__c4d_add`, and the same for `sub`, `mul`, `div`, `sqrt`, `cmp`, `neg` and `abs`;
- conversions to and from `int`, `unsigned`, `long long` and `unsigned long long`;
- conversions between the two widths.

**How it behaves:**
- Every result is correctly rounded: to nearest, ties to even.
- Subnormals, signed zeros, infinities and NaNs all behave as IEEE says.
- NaNs follow x86 SSE: the first NaN operand comes back quieted, and an invalid operation gives the negative quiet "default NaN".
- A conversion to an integer that doesn't fit gives the most negative value, or all ones for unsigned.
- There are no exception flags.

**Where the algorithms come from:**
- Add, subtract, multiply and the conversions follow Berkeley SoftFloat 3.
- Divide and square root are exact digit-at-a-time loops instead of SoftFloat's reciprocal tables.

## Tests: `make test-c4fp`

| file | what |
|---|---|
| `tests/fphost.c` | gcc builds softfp.c and checks every operation bit for bit against the host's SSE. The inputs are edge values crossed with each other, then millions of random operands biased toward cancellation, subnormals, halfway cases and integers. It finds a deliberately broken tie-to-even at once. |
| `tests/fpgen.c` | writes about 6,500 test vectors from the host's own results: one line per case, `OP A B RESULT` in hex. |
| `tests/fpvm.c` | softfp.c compiled by c4lc, run over those vectors at 32 and 64 bits, plain and `-O`. It must get all of them. |
