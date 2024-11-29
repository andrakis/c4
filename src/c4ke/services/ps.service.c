// C4KE Service: process listing
//
// Maintains its own process listing state so that ps requests
// have a consistent count of process details.

#include "c4.h"
#include "c4m.h"

#define PS_NOMAIN 1
#include "ps.c"


