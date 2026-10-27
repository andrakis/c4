// notepad.c -- NOTEPAD.C4R, grin's Notepad (docs/grin-plan.md 4): the
// window kit's Notepad class (src/winkit/notepad.c), which is also the
// C4IX desktop's Text Editor, linked with import.c. Its Open and Save As
// are the kit's file box, whose folders are C4DOS's drives (dospath.h);
// a new document is offered a place on the home drive, where C4DOS
// writes files and the page keeps them.
//
//   NOTEPAD [file]        B:NOTE.TXT, RAM:X, or a name DOS looks for

#include <stdio.h>
#include "winkit.h"
#include "grin.h"
#include "dospath.h"

extern int *__c4dos_api;              // bare.c's: C4DOS's table

enum { SLOT_VERSION = 5, SLOT_HOME = 28 };

static char home[8], untitled[24], path[WK_PATH];

// C4DOS's home drive (slot 28), or -1
static int home_drive() {
    int (*f)(int);
    if (!__c4dos_api || __c4dos_api[SLOT_VERSION] < 5 || !__c4dos_api[SLOT_HOME]) return 0 - 1;
    f = (int (*)(int))__c4dos_api[SLOT_HOME];
    return f(0);
}

int main(int argc, char **argv) {
    int d;
    if (!grin_present()) { printf("Notepad is a grin application: GRIN starts grin, and Notepad is in its Main group.\n"); return 1; }
    notepad_filebox = 1;
    d = home_drive();
    if (d >= 0) {
        home[0] = '/'; home[1] = 'A' + d; home[2] = ':'; home[3] = '/'; home[4] = 0;
        ui_cpy(untitled, home);
        ui_cat(untitled, "UNTITLED.TXT");
        notepad_home = home;
        notepad_untitled = untitled;
    } else {
        notepad_home = "/RAM:/";
        notepad_untitled = "/RAM:/UNTITLED.TXT";
    }
    if (notepad_open(argc > 1 ? grin_kitpath(argv[1], path, WK_PATH) : 0) < 0) return 1;
    return GRIN_RESIDENT;
}
