int main() { char *a[2]; char **pp; a[0] = "x"; a[1] = "y"; pp = a; printf("pp %s\n", pp[1]); return 0; }
