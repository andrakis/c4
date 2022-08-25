//
// SectorLisp ported to C4
//
#include <stdlib.h>
#include <stdio.h>

#pragma GCC diagnostic ignored "-Wformat"

// Keywords
char *S;

// Keyword values as offsets to S
enum {
	kT = 4, kQuote = 6, kCond = 12, kRead = 17, kPrint = 22, kAtom = 28,
	kCar = 33, kCdr = 37, kCons = 41, kEq = 46
};

int cx; // stores negative memory use
int dx; // stores lookahead character
int *RAM; // your own ibm7090

int *M;  // was: #define M (RAM + sizeof(RAM) / sizeof(RAM[0]) / 2)

// Globals to store function pointers
int *ptr_Eval, *ptr_Read, *ptr_Print, *ptr_PrintNewLine;
int *ptr_Cons, *ptr_GetList, *ptr_GetObject, *ptr_PrintObject;
int *ptr_Car,  *ptr_Cdr;
// Also work with normal C compilers through macro magic
#define ptr_Eval(x,y) ((int (*)(int,int))ptr_Eval)(x,y)
#define ptr_Read()    ((int (*)())ptr_Read)()
#define ptr_Print(x)  ((int (*)(int))ptr_Print)()
#define ptr_PrintNewLine() ((int (*)())ptr_PrintNewLine)()
#define ptr_Cons(x,y) ((int (*)(int,int))ptr_Cons)(x,y)
#define ptr_GetList() ((int (*)())ptr_GetList)()
#define ptr_GetObject(x) ((int (*)(int))ptr_GetObject)(x)
#define ptr_PrintObject(x) ((int (*)(int))ptr_PrintObject)(x)
#define ptr_Car(x)    ((int (*)(int))ptr_Car)(x)
#define ptr_Cdr(x)    ((int (*)(int))ptr_Cdr)(x)

int setup_RAM (int sizeof_RAM) {
	if (!(RAM = malloc(sizeof_RAM)))
		return 1;
	return 0;
}
void unsetup_RAM () {
	if (RAM)
		free(RAM);
	RAM = 0;
}
int setup_M (int sizeof_RAM) {
	int r;
	if ((r = setup_RAM(sizeof_RAM)))
		return r;
	M = (RAM + sizeof_RAM / sizeof(int) / 2);
	return 0;
}
void unsetup_M () {
	unsetup_RAM();
	M = 0;
}
void setup_S () {
	S = "NIL\0T\0QUOTE\0COND\0READ\0PRINT\0ATOM\0CAR\0CDR\0CONS\0EQ\0";
}
void unsetup_S () {
	S = 0;
}

int Intern() {
	int i, j, x;
	int _j_flag; // to cover break statement in j loop
	// for (i = 0; (x = M[i++]);)
	i = 0;
	while((x = M[i++])) {
			// for (j = 0;; ++j)
			j = 0;
			_j_flag = 0;
			while(_j_flag == 0) {
				// if (x != RAM[j]) break;
				if (x != RAM[j]) _j_flag = 1;
				else if (!x) return i - j - 1;
				else x = M[i++];
				++j;
			}
			while(x)
				x = M[i++];
	}
	j = 0;
	x = --i;
	while ((M[i++] = RAM[j++]));
	return x;
}

char *l, *p;
int GetChar() {
	int c, t;
}
int setup_GetChar() {
	l = p = 0;
	return 0;
}

int PrintChar(int b) {
	//fputwc(b, stdout);
	return printf("%c", b);
}

int GetToken() {
	int c, i;
	i = 0;
	//do if ((c = GetChar()) > ' ') RAM[i++] = c;
	//while (c <= ' ' || (c > ')' && dx > ')'));
	if ((c = GetChar()) > ' ') RAM[i++] = c;
	while (c <= ' ' || (c > ')' && dx > ')'))
		if ((c = GetChar()) > ' ') RAM[i++] = c;
	RAM[i] = 0;
	return c;
}

int AddList(int x) {
	return ptr_Cons(x, ptr_GetList());
}

int GetList() {
	int c;
	c = GetToken();
	if (c == ')') return 0;
	return AddList(ptr_GetObject(c));
}

int GetObject(int c) {
	if (c == '(') return GetList();
	return Intern();
} 
int Read() {
	return GetObject(GetToken());
}

int PrintAtom(int x) {
	int c;
	int _break;
	_break = 0;
	//for (;;) {
	while(_break == 0) {
		if (!(c = M[x++])) _break = 1;
		else PrintChar(c);
		//}
	}
	return _break;
}

int PrintList(int x) {
	int _break;

	PrintChar('(');
	ptr_PrintObject(ptr_Car(x));
	_break = 0;
	while (!_break && (x = ptr_Cdr(x))) {
		if (x < 0) {
			PrintChar(' ');
			ptr_PrintObject(ptr_Car(x));
		} else {
			PrintChar('-');
			ptr_PrintObject(x);
			_break = 1;
		}
	}
	return PrintChar(')');
}

int PrintObject(int x) {
	return x < 0 ? PrintList(x) : PrintAtom(x);
	//if (x < 0) {
	//	PrintList(x);
	//} else {
	//	PrintAtom(x);
	//}
}

int Print(int e) { return PrintObject(e); }
int PrintNewLine() { return PrintChar('\n'); }


/*───────────────────────────────────────────────────────────────────────────│─╗
│ The LISP Challenge § Bootstrap John McCarthy's Metacircular Evaluator    ─╬─│┼
╚────────────────────────────────────────────────────────────────────────────│*/

int Car(int x) {
	return M[x];
}

int Cdr(int x) {
	return M[x + 1];
}

int Cons(int car, int cdr) {
	M[--cx] = cdr;
	M[--cx] = car;
	return cx;
}

int Gc(int x, int m, int k) {
	return x < m ? Cons(Gc(Car(x), m, k), 
			Gc(Cdr(x), m, k)) + k : x;
}

int Evlis(int m, int a) {
	int x;
	if (m) {
		x = ptr_Eval(Car(m), a);
		return Cons(x, Evlis(Cdr(m), a));
	} else {
		return 0;
	}
}

int Pairlis(int x, int y, int a) {
	return x ? Cons(Cons(Car(x), Car(y)),
			Pairlis(Cdr(x), Cdr(y), a)) : a;
}

int Assoc(int x, int y) {
	if (!y) return 0;
	if (x == Car(Car(y))) return Cdr(Car(y));
	return Assoc(x, Cdr(y));
}

int Evcon(int c, int a) {
	if (ptr_Eval(Car(Car(c)), a)) {
		return ptr_Eval(Car(Cdr(Car(c))), a);
	} else {
		return Evcon(Cdr(c), a);
	}
}

int Apply(int f, int x, int a) {
	if (f < 0)       return ptr_Eval(Car(Cdr(Cdr(f))), Pairlis(Car(Cdr(f)), x, a));
	if (f > kEq)     return Apply(ptr_Eval(f, a), x, a);
	if (f == kEq)    return Car(x) == Car(Cdr(x)) ? kT : 0;
	if (f == kCons)  return Cons(Car(x), Car(Cdr(x)));
	if (f == kAtom)  return Car(x) < 0 ? 0 : kT;
	if (f == kCar)   return Car(Car(x));
	if (f == kCdr)   return Cdr(Car(x));
	if (f == kRead)  return Read();
	if (f == kPrint) return (x ? Print(Car(x)) : PrintNewLine());
}

int Eval(int e, int a) {
	int A, B, C;
	if (e >= 0)
		return Assoc(e, a);
	if (Car(e) == kQuote)
		return Car(Cdr(e));
	A = cx;
	if (Car(e) == kCond) {
		e = Evcon(Cdr(e), a);
	} else {
		e = Apply(Car(e), Evlis(Cdr(e), a), a);
	}
	B = cx;
	e = Gc(e, A, A - B);
	C = cx;
	while (C < B)
		M[--A] = M[--B];
	cx = A;
	return e;
}

int c4_strlen_doublenul(char *s1) {
	char *s2;
	s2 = s1;
	while(!(!*s2 && !*(s2 + 1)))
		++s2;
	return s2 - s1;
}

int setup (int ramwords) {
	int s;
	if ((s = setup_M(ramwords * sizeof(int))))
		return s;
	if ((s = setup_GetChar())) {
		unsetup_M();
		return s;
	}
	setup_S();
	// Setup pointers
	ptr_Eval = (int*)&Eval;
	ptr_Read = (int*)&Read;
	ptr_Print= (int*)&Print;
	ptr_PrintNewLine = (int*)&PrintNewLine;
	ptr_Cons = (int*)&Cons;
	ptr_GetList = (int*)&GetList;
	ptr_GetObject = (int*)&GetObject;
	ptr_PrintObject = (int*)&PrintObject;
	ptr_Car = (int*)&Car;
	ptr_Cdr = (int*)&Cdr;
	//for(i = 0; i < c4_strlen(S) + 1; ++i) M[i] = S[i];
	s = 0;
	printf("strlen=%ld\n", c4_strlen_doublenul(S));
	while(s < c4_strlen_doublenul(S) + 1) { M[s] = S[s]; ++s; }
	return 0;
}

void unsetup () {
	unsetup_M();
	unsetup_S();
}

int main (int argc, char **argv) {
	int ramwords;
	int s, _break;

	ramwords = 32768; // 0100000;

	if ((s = setup(ramwords))) {
		printf("Initialization failure: %ld\n", s);
		return s;
	}

	printf("C4 Lisp [%s]\n", S);

	// do main
	_break = 0;
	while(!_break) {
		cx = 0;
		Print(Eval(Read(), 0));
		PrintNewLine();
	}

	unsetup();
	return 0;
}
