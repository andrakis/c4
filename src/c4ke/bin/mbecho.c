// mbecho.c - a C4KE task bound to the host mailbox: every host frame comes
// back with its type | 256 and each payload word + 1; a type-0 frame ends it.
// The idle time between frames is the kernel's, not a spin: OP_MBOX_AWAIT.

int main (int argc, char **argv) {
	int buf[80], out[64], n, i, echoed;
	if (!mbox_info()) { printf("mbecho: no host mailbox\n"); return 1; }
	printf("mbecho: bound\n");
	echoed = 0;
	while (1) {
		mbox_await(0);
		n = mbox_recv(buf, 80);
		if (n < 3) continue;
		if (buf[1] == 0) break;
		i = 0;
		while (i < n - 3) { out[i] = buf[3 + i] + 1; i = i + 1; }
		mbox_send(buf[1] | 256, out, n - 3);
		++echoed;
	}
	printf("mbecho: done, %d frames\n", echoed);
	return 0;
}
