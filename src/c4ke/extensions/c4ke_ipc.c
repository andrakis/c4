// C4KE extension: InterProcess Communication
//
// Very primitive. Currently uses signals instead of opcodes.
// Mostly because I wanted to see if it was possible to transfer data via ipc_setup_signal.
//
// To transfer a word:
//   1. TASK1: Send TASK2 a SIGIO ipc_setup_signal
//      TASK1: Wait for SIGCONT
//   2. TASK2 receives SIGIO, enters word wait state
//      TASK2 sends SIGCONT to TASK1.
//   3. TASK1: Receives SIGCONT
//      TASK1: Sends SIGRTMIN+x to TASK2
//      TASK1 can continue with normal processing
//   4. TASK2: Receives SIGRTMIN+x, puts x into received buffer
//      TASK2 can continue with normal processing until the next SIGIO
//
// To transfer a packet:
//   1. Set SendPktSize = total_bytes / (word_size / (ipc_bits / 8))
//   2. TASK1 sends packet size to TASK2 and waits for response
//   3. TASK2 sends response packet 0
//   4. TASK1 sends current packet and waits for response
//   5. TASK1: If more packets to send, goto 2
//   6. TASK2: Receives packet and adds it to buffer until packets received


#include <c4ke/extension.h>

static int ipc_enabled, ipc_bits, ipc_mask, ipc_xfers_fullword, ipc_stop_bits;

// Count the number of of bits until the first on bit
static int countFirstSetBit (int number) {
	int bits;
	bits = 0;
	while(number) {
		if (!(number & 1)) {
			++bits;
			number = number >> 1;
		} else {
			return bits;
		}
	}
	return bits;
}

// Test tasks and associated state and data

enum {
	SM_NONE,
	SM_TRANSMIT,
	SM_WAITCONT,
	SM_DONE
};

enum {
	RM_NONE,
	RM_TRANSMIT,
	RM_WAITWORD,
	RM_DONE
};

static int test_task_1_id, test_task_2_id;
static int task1_keep_running, task2_keep_running;

static int  sender_mode, receiver_mode;
static int  sender_word, receiver_word;
static int  sender_words_left, receiver_words_left;

static void test_task1_SIGTERM () { task1_keep_running = 0; }
static void test_task2_SIGTERM () { task2_keep_running = 0; }

static void ipc_send_signal (int pid, int sig) {
	__c4_opcode(sig, pid, OP_USER_KILL);
}
static int ipc_setup_signal (int sig, int *handler) {
	return __c4_opcode(handler, sig, OP_USER_SIGNAL);
}

static int ipc_reverse_word (int n) {
	int res, i;
	res = 0;
	i = sizeof(int) * 8;
	while (i--) {
		res = (res << 1) + (n % 2);
		n = n >> 1;
	}
	return res;
}

// TODO: this doesn't appear to be working correctly
static void ipc_print_nbits_old (int word, int nbits) {
	int i;
	i = 0;
	//word = ipc_reverse_word(word);
	while (nbits > 0) {
		printf("%d", (word >> nbits) & 1);
		--nbits;
		if (++i % (ipc_bits + 0) == 0)
			printf(" ");
	}
}
static char *ipc_print_buffer;
static int   ipc_print_buffer_len;
static void ipc_print_nbits (int number, int bits) {
	char *buffer;
	int   i, spacing;

	buffer = ipc_print_buffer;
	memset(buffer, 0, ipc_print_buffer_len);
	buffer = buffer + (bits - 1) + ((bits - 1) / ipc_bits);
	i = bits - 1;
	spacing = 0;
	while (i >= 0) {
		*buffer-- = (number & 1) + '0';
		--i;
		number = number >> 1;
		if (++spacing % ipc_bits == 0)
			*buffer-- = ' ';
	}
	printf("%s", ipc_print_buffer);
}
static void ipc_print_bits (int word) {
	ipc_print_nbits(word, ipc_bits);
}
static void ipc_print_fullword (int word) {
	ipc_print_nbits(word, (sizeof(int) * 8));
}

static void sender_send_fullword (int fullword, int pid) {
	printf("ipc0: initiating send_fullword to pid %d, value: 0x%x (%d)\n"
	       "ipc0: full word bits: ",
	       pid, fullword, fullword);
	ipc_print_fullword(fullword);
	sender_word = fullword; //ipc_reverse_word(fullword);
	printf("\nipc0: transmit buffer: ");
	ipc_print_fullword(sender_word);
	printf("\n");
	sender_words_left = ipc_xfers_fullword;
	sender_mode = SM_TRANSMIT;
	ipc_send_signal(test_task_2_id, SIGIO);
	sender_mode = SM_WAITCONT;
}

static void sender_sigcont () {
	int partial_word, rt, i, mask;
	// TODO: we cannot get the ipc_setup_signal information to reply to
	//       whoever sent the ipc_setup_signal. Assume it's task2.
	if (sender_mode != SM_WAITCONT)
		printf("ipc0: error, SIGCONT encountered outside of WAITCONT state value: %d\n", sender_mode);
	if (sender_words_left < 0) {
		printf("ipc0: sender words remain underflowed, aborting\n");
		sender_mode = SM_NONE;
		return;
	}
	printf("ipc0: SIGCONT received with %d partial words remaining to send\n", sender_words_left);
	sender_mode = SM_TRANSMIT;
	// Move ipc_bits of sender_word into partial_word
	//printf("ipc0: grabbing %d bits from current word 0x%lx (%ld) using mask 0x%x\n"
	//       "ipc0: transmit buffer: ", ipc_bits, sender_word, sender_word, ipc_mask);
	//ipc_print_nbits(sender_word & mask, ipc_bits);
	//printf("\n");
	//partial_word = 0;
	//i = ipc_bits;
	// TODO: is not grabbing partial_word correctly, gets 0110 from 1011
	//       bits are inverted.
	//while (i) {
		// Grab a bit
		//partial_word = (partial_word << 1) | (sender_word & 1 ? 1 : 0); // TODO: here?
	//	partial_word = (partial_word << 1) | ((sender_word >> i) & 1) ? 1 : 0;
		// Move to next bit
		//sender_word = (sender_word >> 1);
	//	--i;
	//}
	partial_word = sender_word & ipc_mask;
	//partial_word = ipc_reverse_word(partial_word);
	sender_word = sender_word >> ipc_bits;
	printf("ipc0: grabbed %d bits, value 0x%lx (%ld), bits: ", ipc_bits, partial_word, partial_word);
	ipc_print_nbits(partial_word, ipc_bits);
	printf("\n");
	rt = SIGRTMIN + partial_word;
	sender_mode = SM_TRANSMIT;
	printf("ipc0: partial bits to send: ");
	ipc_print_bits(partial_word);
	printf(" 0x%x (%d), signal value 0x%x (%d)\n",
	       partial_word, partial_word, rt, rt);
	ipc_send_signal(test_task_2_id, rt);
	if (--sender_words_left == 0) {
		printf("ipc0: full word transmitted, remaining in buffer: 0x%lx\n", sender_word);
		sender_mode = SM_DONE;
	} else if (sender_words_left) {
		printf("ipc0: word transmitted, %d words remain, word remaining in buffer: 0x%lx\n"
		       "ipc0: remaining word bits: ",
		       sender_words_left, sender_word);
		ipc_print_fullword(sender_word);
		printf("\n");
		sender_mode = SM_WAITCONT;
	} else {
		printf("ipc0: error, word transmit underflow, word remaining in buffer: 0x%lx\n",
		       sender_word);
		sender_mode = SM_NONE;
	}
}

static void receiver_sigio () {
	if (receiver_mode != RM_NONE)
		printf("ipc1: error, SIGIO encountered when mode (%d) != NONE\n", receiver_mode);
	receiver_mode = RM_TRANSMIT;
	ipc_send_signal(test_task_1_id, SIGCONT);
	receiver_mode = RM_WAITWORD;
	receiver_word = 0;
	receiver_words_left = ipc_xfers_fullword;
	printf("ipc1: SIGIO received, awaiting %d words\n", receiver_words_left);
}

// TODO: this signature is needed to get access to the ipc_setup_signal number, but it's ugly.
static void receiver_sigrt (int sig, int a, int b, int c, int d) {
	int word;
	if (receiver_mode != RM_WAITWORD)
		printf("ipc1: error, SIGRTMIN+%d encountered when mode (%d) != WAITWORD\n", receiver_mode);
	word = sig - SIGRTMIN;
	printf("ipc1: receive partial word ");
	ipc_print_bits(word);
	printf(" %d (0x%x), %d words remain\n", word, word, receiver_words_left - 1);
	if (receiver_words_left) {
		receiver_word = (receiver_word << ipc_bits) | word;
		printf("ipc1: updated full word to 0x%lx (%d)\n"
		       "ipc1: full word currently: ",
		       receiver_word, receiver_word);
		ipc_print_fullword(receiver_word);
		//ipc_print_nbits(receiver_word, -1 + ipc_bits * (ipc_xfers_fullword - (receiver_words_left - 1)));
		printf("\n");
		if (--receiver_words_left) {
			printf("ipc1: sending SIGCONT and awaiting next partial word\n");
			receiver_mode = RM_TRANSMIT;
			ipc_send_signal(test_task_1_id, SIGCONT);
			receiver_mode = RM_WAITWORD;
		} else {
			receiver_word = ipc_reverse_word(receiver_word);
			printf("ipc1: full word received: 0x%lx (%ld)\nipc1: ", receiver_word, receiver_word);
			ipc_print_fullword(receiver_word);
			printf("\n");
			receiver_mode = RM_NONE;
		}
	} else if (!receiver_words_left) {
		printf("ipc1: error, receiver_words_left underflowed\n");
		receiver_words_left = 0;
	}
}

static int test_task_1 (int argc, char **argv) {
	test_task_1_id = __c4_opcode(OP_USER_PID);
	ipc_setup_signal(SIGTERM, (int *)&test_task1_SIGTERM);

	printf("ipc0: test_task_1 starting, pid %d\n", test_task_1_id);
	printf("ipc0: waiting for second task...\n");
	while (test_task_2_id == 0) {
		__c4_opcode(1, OP_USER_SLEEP);
	}

	task1_keep_running = 1;
	ipc_setup_signal(SIGCONT, (int *)&sender_sigcont);
	//sender_word = 0x12345678;
	sender_word = 123;
	printf("ipc0: sending a word: 0x%lx (%ld)\n", sender_word, sender_word);
	sender_send_fullword(sender_word, test_task_2_id);
	printf("ipc0: entering message loop...\n");
	while (task1_keep_running) {
		__c4_opcode(500, OP_USER_SLEEP);
	}

	printf("ipc0: test_task_1 finishing\n");
	return 0;
}
static int test_task_2 (int argc, char **argv) {
	int i;

	test_task_2_id = __c4_opcode(OP_USER_PID);
	ipc_setup_signal(SIGTERM, (int *)&test_task2_SIGTERM);

	printf("ipc1: test_task_2 starting, pid %d\n", test_task_2_id);
	printf("ipc1: waiting for first task...\n");
	while (test_task_1_id == 0) {
		__c4_opcode(1, OP_USER_SLEEP);
	}

	task2_keep_running = 1;
	// Setup SIGIO
	ipc_setup_signal(SIGIO, (int *)&receiver_sigio);
	// Setup all SIGRTMIN+x handlers
	i = SIGRTMIN;
	while (i <= SIGMAX) {
		ipc_setup_signal(i, (int *)&receiver_sigrt);
		++i;
	}
	printf("ipc1: entering message loop...\n");
	while (task2_keep_running) {
		__c4_opcode(500, OP_USER_SLEEP);
	}

	printf("ipc1: test_task_2 finishing\n");
	return 0;
}

// Records our start position in the extended data segment of tasks
static int ipc_extdata_start;

static int ipc_init () {
	int ws, xfers, i;

	// Use a buffer of sizeof(int) * 9, instead of * 8, for writing spacing
	if (!(ipc_print_buffer = malloc((ipc_print_buffer_len = sizeof(int) * sizeof(int))))) {
		printf("c4ke: ipc module failed to allocate memory\n");
		return KXERR_FAIL;
	}
	memset(ipc_print_buffer, 0, ipc_print_buffer_len);

	ipc_stop_bits = 0;
	ipc_bits = countFirstSetBit(SIGMAX - SIGRTMIN) - ipc_stop_bits;
	// ensure word size fits into ipc_bits evenly
	ws = sizeof(int) * 8;
	while (ws % ipc_bits != 0)
		--ipc_bits;
	ipc_xfers_fullword = ws / ipc_bits;
	ipc_mask = 1;
	i = ipc_bits;
	while(--i) ipc_mask = (ipc_mask << 1) | 1;
	if (kernel_verbosity >= VERB_MED)
		printf("c4ke: ipc module loaded, communication bits %d + %d stop bits, range %d - %d, %d xfers per machine word\n",
		       ipc_bits, ipc_stop_bits, SIGRTMIN, SIGRTMAX, ipc_xfers_fullword);
	ipc_enabled = 1;

	// Our data offset in the ext data is the current value
	ipc_extdata_start = kernel_task_extdata_size;
	// Reserve enough space for our structure
	kernel_task_extdata_size = kernel_task_extdata_size + sizeof(int *);
	return KXERR_NONE;
}

static int ipc_start () {
	int *t1, *t2;
	char **tmp_argv;
	if (!(tmp_argv = malloc(sizeof(char **) * 1)))
		return KXERR_FAIL;
	*tmp_argv = "ipc test";
	// TODO: not working correctly, memory corruption, etc
	// also, -1 >> 1 keeps the - value!
	//t1 = start_task_builtin((int *)&test_task_1, 1, tmp_argv, "ipc: test 1", PRIV_USER);
	//t2 = start_task_builtin((int *)&test_task_2, 1, tmp_argv, "ipc: test 2", PRIV_USER);
	free(tmp_argv);
	return KXERR_NONE;
}

static int ipc_shutdown () {
	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: ipc module shutdown\n");
	return KXERR_NONE;
}

static int __attribute__((constructor)) ipc_constructor () {
	kext_register("ipc", (int *)&ipc_init, (int *)&ipc_start, (int *)&ipc_shutdown);
}
