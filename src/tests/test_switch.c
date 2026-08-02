// C4 Test: switch/case/default with a jumptable, and break in loops.
// Exercises: dense and offset case ranges, negative cases, fallthrough,
// break, default, out-of-range values both sides, enum constants as
// cases, nested switch, switch inside a loop with break, and break in
// plain while/for loops.
enum { RED = 10, GREEN = 11, BLUE = 12 };

int classify (int x) {
	switch (x) {
	case 0: return 100;
	case 1:
	case 2: return 200;      // fallthrough from 1
	case 5: return 500;      // hole at 3,4
	default: return -1;
	}
}

int colour (int c) {
	int r;
	r = 0;
	switch (c) {
	case RED:   r = 1; break;
	case GREEN: r = 2; break;
	case BLUE:  r = 3; break;
	}
	return r;                // no default: unmatched falls through
}

int negatives (int x) {
	switch (x) {
	case -2: return 22;
	case 0:  return 0;
	case 3:  return 33;
	}
	return 99;
}

int nested (int a, int b) {
	switch (a) {
	case 1:
		switch (b) {
		case 1: return 11;
		case 2: return 12;
		}
		return 10;
	case 2: return 20;
	}
	return 0;
}

int main () {
	int i, sum;
	i = 0;
	while (i <= 6) {
		printf("classify(%d) = %d\n", i, classify(i));
		i = i + 1;
	}
	printf("colour: %d %d %d %d\n", colour(RED), colour(GREEN), colour(BLUE), colour(42));
	printf("negatives: %d %d %d %d %d\n",
	       negatives(-3), negatives(-2), negatives(0), negatives(3), negatives(7));
	printf("nested: %d %d %d %d %d\n",
	       nested(1, 1), nested(1, 2), nested(1, 9), nested(2, 0), nested(9, 9));
	// break inside while
	i = 0; sum = 0;
	while (1) {
		if (i >= 10) break;
		sum = sum + i;
		i = i + 1;
	}
	printf("while break: %d\n", sum);
	// switch inside a loop, break binds to the switch
	i = 0; sum = 0;
	while (i < 5) {
		switch (i) {
		case 2: sum = sum + 100; break;
		case 4: sum = sum + 200; break;
		default: sum = sum + 1; break;
		}
		i = i + 1;
	}
	printf("loop switch: %d\n", sum);
	return 0;
}
