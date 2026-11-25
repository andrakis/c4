// c4tui demo: every widget the library has, driven from a menu bar.
//
// Built with c4lc or c4fc, which have a real preprocessor:
//   ./c4sp src/c4sp/lisp/c4lc.lisp -O -conforming -P -I src/c4tui \
//          src/c4tui/demo.c c4tui-demo.c4r
// With c4cc, which has none, drop the #include and pass both files:
//   ./c4cc -o c4tui-demo.c4r src/c4tui/c4tui.c src/c4tui/demo.c
#include "c4tui.c"

//
// It is also the manual test. Run it under c4m, or on c4bb with
//   node src/c4bb/sim/cli.js -i -d <disk> c4tui-demo.c4r

int  m_names[4];
int  m_file[5];
int  m_edit[4];
int  m_help[2];
int  d_btn[2];
int  l_items[8];
char namebuf[64];

int A_DESK, A_BAR, A_BARSEL, A_WIN, A_SEL, A_STAT;

void draw (int menu, char *note) {
  tui_cls(A_DESK);
  tui_menubar(0, m_names, 4, menu, A_BAR, A_BARSEL);
  tui_window(4, 3, 60, 14, " c4tui ", A_WIN);
  tui_text(7, 5, "A character-cell UI library for C4DOS.", A_WIN);
  tui_text(7, 7, "Alt-F / Alt-E / Alt-H open a menu.", A_WIN);
  tui_text(7, 8, "F2 a dialog, F3 a list, F4 an input field.", A_WIN);
  tui_text(7, 9, "Esc quits.", A_WIN);
  tui_text(7, 11, "name:", A_WIN);
  tui_text(13, 11, namebuf, A_SEL);
  if (note) tui_text(7, 13, note, A_WIN);
  tui_fill(0, tui_h - 1, tui_w, 1, ' ', A_STAT);
  tui_text(1, tui_h - 1, "F2 dialog  F3 list  F4 input  Esc quit", A_STAT);
}

int main () {
  int k, n, menu;

  tui_init(80, 25);
  A_DESK   = tui_attr(TUI_LGREY,  TUI_BLUE);
  A_BAR    = tui_attr(TUI_BLACK,  TUI_LGREY);
  A_BARSEL = tui_attr(TUI_WHITE,  TUI_GREEN);
  A_WIN    = tui_attr(TUI_YELLOW, TUI_BLUE);
  A_SEL    = tui_attr(TUI_BLUE,   TUI_LGREY);
  A_STAT   = tui_attr(TUI_BLACK,  TUI_LGREY);

  m_names[0] = (int)"File"; m_names[1] = (int)"Edit";
  m_names[2] = (int)"Run";  m_names[3] = (int)"Help";
  m_file[0] = (int)"New";  m_file[1] = (int)"Open...";
  m_file[2] = (int)"-";    m_file[3] = (int)"Save";  m_file[4] = (int)"Exit";
  m_edit[0] = (int)"Cut";  m_edit[1] = (int)"Copy";
  m_edit[2] = (int)"Paste"; m_edit[3] = (int)"Find...";
  m_help[0] = (int)"About"; m_help[1] = (int)"Index";
  d_btn[0] = (int)"OK";    d_btn[1] = (int)"Cancel";
  l_items[0] = (int)"editor.c";  l_items[1] = (int)"c4tui.c";
  l_items[2] = (int)"demo.c";    l_items[3] = (int)"README";
  l_items[4] = (int)"makefile";  l_items[5] = (int)"notes.txt";
  l_items[6] = (int)"scratch.c"; l_items[7] = (int)"old.bak";
  namebuf[0] = 0;

  menu = 0 - 1;
  draw(menu, 0);
  tui_flush();

  while (1) {
    k = tui_key();
    if (k == TUI_ESC) break;

    if (k == TUI_ALT + 'f' || (k == TUI_F10)) {
      draw(0, 0); tui_flush();
      n = tui_popup(tui_menux(m_names, 4, 0), 1, m_file, 5, 0, A_WIN, A_SEL);
      if (n == 4) break;
      draw(0 - 1, n < 0 ? "File: cancelled" : "File: chose an item");
    } else if (k == TUI_ALT + 'e') {
      draw(1, 0); tui_flush();
      n = tui_popup(tui_menux(m_names, 4, 1), 1, m_edit, 4, 0, A_WIN, A_SEL);
      draw(0 - 1, n < 0 ? "Edit: cancelled" : "Edit: chose an item");
    } else if (k == TUI_ALT + 'h') {
      draw(3, 0); tui_flush();
      n = tui_popup(tui_menux(m_names, 4, 3), 1, m_help, 2, 0, A_WIN, A_SEL);
      draw(0 - 1, n < 0 ? "Help: cancelled" : "Help: chose an item");
    } else if (k == TUI_F2) {
      n = tui_dialog(" Confirm ", "Discard the changes?", d_btn, 2, A_WIN, A_SEL);
      draw(0 - 1, n == 0 ? "dialog: OK" : (n == 1 ? "dialog: Cancel" : "dialog: Esc"));
    } else if (k == TUI_F3) {
      n = tui_list(20, 5, 34, 12, " Open ", l_items, 8, 0, A_WIN, A_SEL);
      draw(0 - 1, n < 0 ? "list: cancelled" : "list: chose a file");
    } else if (k == TUI_F4) {
      draw(0 - 1, "type a name, Enter or Esc");
      tui_flush();
      tui_input(13, 11, 30, namebuf, 60, A_SEL);
      draw(0 - 1, "input: done");
    } else {
      draw(0 - 1, 0);
    }
    tui_flush();
  }

  tui_end();
  printf("c4tui demo: bye\n");
  return 0;
}
