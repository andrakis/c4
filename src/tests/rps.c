// C4KE Game: Rock Paper Scissors
//
// Honest: CPU move is calculated before asking for player input.
// Very simple algorithm: CPU picks the least used move over the last X moves.
// Occasionally it will pick a random choice not governed by the players moves.

#include <ctype.h>
#include <stdio.h>

#include "u0.h"
#include "string.h"

// Configuration
enum {
	// How much history to keep and examine for moves.
	// Affects CPU behaviour, CPU will pick the least used move over the last X moves.
	MAX_HISTORY = 5
};

// Moves
enum { MOVE_NONE, MOVE_ROCK, MOVE_PAPER, MOVE_SCISSORS, MOVES, MOVE_QUIT };
// Player
enum { NONE, CPU, PLAYER };

// Streams
enum { STDIN, STDOUT, STDERR };

// Input handling
enum { INPUT_BUFFER_SZ = 256 };
char *input_buffer, *input_buffer_ptr;
int  input_len;
// places result into input_buffer
int  read_player_input (char *prompt) {
	if (input_buffer == 0) {
		if (!(input_buffer = malloc(INPUT_BUFFER_SZ))) {
			printf("Memory allocation error for input buffer.\n");
			exit(-2);
		}
	}
	memset(input_buffer, 0, INPUT_BUFFER_SZ);
	printf("%s\n", prompt);
	input_buffer_ptr = input_buffer;
	return input_len = read(STDIN, input_buffer, INPUT_BUFFER_SZ);
}
char *input_next () {
	int remain;
	remain = input_len - (input_buffer_ptr - input_buffer);
	if (remain <= 0) return 0;
	while (remain && !isalpha(*input_buffer_ptr)) {
		++input_buffer_ptr;
		--remain;
	}
	return input_buffer_ptr;
}

// Utility
void print_move (int move) {
	char *m;

	if (move == MOVE_NONE) m = "NONE";
	else if (move == MOVE_ROCK) m = "ROCK";
	else if (move == MOVE_PAPER) m = "PAPER";
	else if (move == MOVE_SCISSORS) m = "SCISSORS";
	else if (move == MOVE_QUIT) m = "QUIT";
	else {
		printf("INVALID MOVE DETECTED: %d\n", move);
		return;
	}

	printf("%s", m);
}

int is_player_win (int player_move, int cpu_move) {
	if (player_move == MOVE_ROCK && cpu_move == MOVE_SCISSORS)
		return 1;
	else if (player_move == MOVE_PAPER && cpu_move == MOVE_ROCK)
		return 1;
	else if (player_move == MOVE_SCISSORS && cpu_move == MOVE_PAPER)
		return 1;
	return 0;
}

// Player history table
int *player_history, player_history_empty_slots;
void setup_player_history () {
	if (!(player_history = malloc(sizeof(int) * (1 + MAX_HISTORY)))) {
		printf("Memory allocation error\n");
		exit(-1);
	}
	memset(player_history, MOVE_NONE, sizeof(int) * MAX_HISTORY);
	player_history_empty_slots = MAX_HISTORY;
}
void print_player_history () {
	int i;
	printf("Player history table (%d slots free):\n", player_history_empty_slots);
	i = 0;
	while (i < MAX_HISTORY) {
		printf(" %2d | ", i);
		print_move(player_history[i]);
		printf("\n");
		++i;
	}
}
void update_player_history (int move) {
	int really_annoying_index_method;

	if (player_history_empty_slots > 0)
		player_history[MAX_HISTORY - player_history_empty_slots--] = move;
	else {
		// Move everything back one
		// Why the heck does this not work?
		// memmove(player_history, player_history + sizeof(int), sizeof(int) * (MAX_HISTORY - 3));
		really_annoying_index_method = 0;
		while(really_annoying_index_method < MAX_HISTORY - 1) {
			player_history[really_annoying_index_method] = player_history[really_annoying_index_method + 1];
			++really_annoying_index_method;
		}
		// Update latest move
		player_history[MAX_HISTORY - 1] = move;
	}
	// print_player_history();
}

// Player move handling
int get_player_move (int cpu_move) {
	int len;
	char *p, c;

	while (1) {
		len = read_player_input("Your move: [r]ock [p]aper [s]cissors (or [q]uit, or [v]iew my decision)");
		if (len == 0) printf("No input received\n");
		else {
			// Find first non-space
			p = input_next();
			if (*p) {
				c = tolower(*p);
				if (c == 'r') return MOVE_ROCK;
				else if (c == 'p') return MOVE_PAPER;
				else if (c == 's') return MOVE_SCISSORS;
				else if (c == 'q') return MOVE_QUIT;
				else if (c == 'v') { printf("(The CPU has chosen "); print_move(cpu_move); printf(")\n"); }
			}
			printf("I didn't understand that, please try again. ");
		}
	}

	// Never reaches here
}

// CPU move handling
int get_cpu_move () {
	// The CPU chooses its move based on previous player moves.
	// If there are none, it uses rand() for a random number and uses
	// that to choose which move to make.
	// There is always a chance, based on the random number generator, to pick a
	// move completely at random.
	int r, mr, mp, ms, i, m;

	r = rand() % 101; // Change to 0 - 100 range
	if (r < 0) r = -r;
	// 10% chance for completely random move
	if (r <= 10) {
		printf("CPU: You won't expect this!\n");
		return MOVE_ROCK + (r % 3); // Clamp to move table
	}

	// Count up the various move counts
	mr = mp = ms = i = 0;
	while(i < MAX_HISTORY) {
		m = player_history[i];
		if (m == MOVE_ROCK) ++mr;
		else if (m == MOVE_PAPER) ++mp;
		else if (m == MOVE_SCISSORS) ++ms;
		++i;
	}

	if (mr > mp && mr > ms) // Rock used most
		return MOVE_PAPER;
	if (mp > mr && mp > ms) // Paper used most
		return MOVE_SCISSORS;
	if (ms > mr && ms > mp) // Scissors used most
		return MOVE_ROCK;
	if (mr == 0)
		return MOVE_ROCK;  // Rock not used recently
	else if (mp == 0)
		return MOVE_PAPER; // ..
	else if (ms == 0)
		return MOVE_SCISSORS; // ..
	//printf("CPU: Not sure what move to do based on player history[%d, %d, %d], choosing ROCK\n", mr, mp, ms);
	return MOVE_ROCK;
}

// Singleplayer vs CPU mode
// returns CPU or PLAYER, whoever wins
int singleplayer_round () {
	int player_move, cpu_move;

	player_move = MOVE_NONE;
	while (player_move != MOVE_QUIT) {
		cpu_move = MOVE_NONE;
		// Err...somehow cpu_move sometimes is NONE
		while (cpu_move == MOVE_NONE)
			cpu_move = get_cpu_move();
		printf("I've picked my move, what is yours? ");
		player_move = get_player_move(cpu_move);
		update_player_history(player_move);
		if (player_move == MOVE_QUIT)
			return NONE;
		if (player_move == cpu_move) {
			printf("Draw! We both picked ");
			print_move(player_move);
			printf(" Let's try again.\n");
		} else if (is_player_win(player_move, cpu_move)) {
			printf("Player's ");
			print_move(player_move);
			printf(" beats CPU's ");
			print_move(cpu_move);
			printf(", you win!\n\n");
			return PLAYER;
		} else {
			printf("CPU's ");
			print_move(cpu_move);
			printf(" beats PLAYER's ");
			print_move(player_move);
			printf(", CPU wins!\n\n");
			return CPU;
		}
	}

	return NONE;
}

void singleplayer () {
	int player_wins, cpu_wins, winner, quit;

	player_wins = cpu_wins = quit = 0;

	while(!quit) {
		winner = singleplayer_round();
		if (winner == PLAYER) ++player_wins;
		else if (winner == CPU) ++cpu_wins;
		else quit = 1;
		printf("Player wins: %d  CPU wins: %d\n\n", player_wins, cpu_wins);
	}
}

// Entry point
int main (int argc, char **argv) {
	setup_player_history();

	singleplayer();
	free(player_history);
	free(input_buffer);
	return 0;
}
