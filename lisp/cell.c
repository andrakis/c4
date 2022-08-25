// C4 class: Lisp Cell
//
// Implements a cell that contains any of the following:
//   * Integer
//   * String
//   * Atom (string variant, stores index to atom table. Single integer comparison.)
//   * List (of any of the above types, including List)
#include <stdlib.h>

// enum LispCellType
enum { LispCellType_Atom, LispCellType_Integer, LispCellType_String, LispCellType_List };

// class LispCell {
// 	int type; // LispCellType
// 	int *data;// Pointer to data
// }
enum {
	LispCell_type, LispCell_data
};

// class LispList {
// 	int *head;
// 	int *tail;
// }
enum {
	LispList_head, LispList_tail
};

// class LispAtom {
//   int id;
//   char *string;
// }
enum { LispAtom_Id, LispAtom_String, LispAtom__sz };

// std::dictionary<int,LispAtom> LispAtomsById;
// std::dictionary<string,LispAtom> LispAtomsByName;
int *LispAtomsById, *LispAtomsByName, LispAtomsMax, LispAtomsCount;

int lisp_init () {
	LispAtomsCount = 0;
	return 0;
}
