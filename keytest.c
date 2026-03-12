#include <ncurses.h>

int main () {
	initscr();
	noecho();
	keypad(stdscr, TRUE);

	printw("Press keys to see their codes. ctrl + c to quit.\n");
	refresh();

	while(1) {
		int key = getch();
		printw("Key code: %d\n", key);
		refresh();
}
	endwin();
	return 0;
}
