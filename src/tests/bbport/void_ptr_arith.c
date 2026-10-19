int main() { char buf[8]; void *p; p = buf; p = (char *)p + 3; printf("vp %d\n", (int)((char *)p - buf)); return 0; }
