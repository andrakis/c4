// C4 Stdlib Class: Map
//
// Implements C++'s map<Key,Value> using pair<Key,Value> and vector<pair<Key,Value>>
//

// Invocation: ./c4 c4_multiload.c classes.c stdlib/pair.c stdlib/vector.c stdlib/iterator.c stdlib/map.c
// import: classes.c
// import: stdlib/pair.c
// import: stdlib/iterator.c
// import: stdlib/vector.c

#ifndef _C4STDLIB_MAP
#define _C4STDLIB_MAP 1

#include "classes.c"
#include "stdlib/pair.c"
#include "stdlib/vector.c"
#include "stdlib/lambda.c"
#include "stdlib/iterator.c"

// class C4Map<KeySize,ValueSize> {
//    C4Vector<C4Pair<KeySize,ValueSize>> _map;
//    public:
//    C4Map(int KeySize, int ValueSize, int capacity = 20) {
//        _map = C4Vector_new(sizeof(C4Pair) * capacity);
//    }

int *new_C4Map (int keysize, int valuesize, int capacity) {
	int *ptr;
	if (!capacity) capacity = 20;
	return ptr;
}

#endif
