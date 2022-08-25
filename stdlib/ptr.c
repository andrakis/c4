// C4 Classes: Pointers
//
// Provides 3 classes:
//   C4UniquePtr      like std::unique_ptr
//   C4WeakPtr        like std::weak_ptr
//   C4SharedPtr      like std::shared_ptr
//

#include <stdlib.h>

#define CLASSES_NOMAIN
#include "classes.c"

// class C4UniquePtr {
//   void *_ptr;
// public:
//   C4UniquePtr() : C4UniquePtr(0) {}
//   C4UniquePtr(void *ptr) : _ptr(ptr) {}
//   C4UniquePtr(C4UniquePtr &ptr) : _ptr(0) {
//     swap(_ptr, ptr._ptr);
//   }
//   ~C4UniquePtr() { _ptr ? free(ptr) : 0; }
//   void *Get () { return _ptr; }
// }
