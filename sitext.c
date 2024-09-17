#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SITEXT_VERSION "0.0.1"
#define SITEXT_TAB_STOP 8
#define SITEXT_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

enum editor_key
{
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

typedef struct erow
{
	int size;
	int rsize;
	char *chars;
	char *render;
} erow;

struct editor_config
{
	int cursor_x, cursor_y;
	int render_x;
	int row_off;
	int col_off;
	int screen_rows;
	int screen_cols;
	int num_rows;
	erow *row;
	int dirty;
	char *filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
};

struct editor_config E;

void editor_set_status_message(const char *fmt, ...);
void editor_refresh_screen();
char *editor_prompt(char *prompt);

void die(const char *s)
{
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void disable_raw_mode()
{
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == - 1)
	{
		die("tcsetattr");
	}
}

void enable_raw_mode()
{
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
	{
		die("tcgetattr");
	}
	atexit(disable_raw_mode);

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
	{
		die("tcsetattr");
	}
}

int editor_read_key()
{
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
	{
		if (nread == -1 && errno != EAGAIN)
		{
			die("read");
		}
	}

	if (c == '\x1b')
	{
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1)
		{
			return '\x1b';
		}
		if (read(STDIN_FILENO, &seq[1], 1) != 1)
		{
			return '\x1b';
		}

		if (seq[0] == '[')
		{
			if (seq[1] >= '0' && seq[1] <= '9')
			{
				if (read(STDIN_FILENO, &seq[2], 1) != 1)
				{
					return '\x1b';
				}
				if (seq[2] == '~')
				{
					switch (seq[1])
					{
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}
			}
			else
			{
				switch (seq[1])
				{
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		}
		else if (seq[0] == 'O')
		{
			switch (seq[1])
			{
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}
		return '\x1b';
	}
	else
	{
		return c;
	}
}

int get_cursor_position(int *rows, int *cols)
{
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
	{
		return -1;
	}

	while (i < sizeof(buf) - 1)
	{
		if (read(STDIN_FILENO, &buf[i], 1) != 1)
		{
			break;
		}
		
		if (buf[i] == 'R')
		{
			break;
		}

		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[')
	{
		return -1;
	}

	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
	{
		return -1;
	}

	return 0;
}

int get_window_size(int *rows, int *cols)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
	{
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
		{
			return -1;
		}
		return get_cursor_position(rows, cols);
	}
	else
	{
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

int editor_row_cursorx_to_renderx(erow *row, int cursor_x)
{
	int render_x = 0;
	int j;
	for (j = 0; j < cursor_x; j++)
	{
		if (row->chars[j] == '\t')
		{
			render_x += (SITEXT_TAB_STOP - 1) - (render_x % SITEXT_TAB_STOP);
		}
		render_x++;
	}
	return render_x;
}

void editor_update_row(erow *row)
{
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++)
	{
		if (row->chars[j] == '\t')
		{
			tabs++;
		}
	}

	free(row->render);
	row->render = malloc(row->size + tabs * (SITEXT_TAB_STOP - 1) + 1);

	int idx = 0;
	for (j = 0; j < row->size; j++)
	{
		if (row->chars[j] == '\t')
		{
			row->render[idx++] = ' ';
			while (idx % SITEXT_TAB_STOP != 0)
			{
				row->render[idx++] = ' ';
			}
		}
		else
		{
			row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editor_insert_row(int at, char *s, size_t len)
{
	if (at < 0 || at > E.num_rows)
	{
		return;
	}

	E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));
	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.num_rows - at));
	
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editor_update_row(&E.row[at]);

	E.num_rows++;
	E.dirty++;
}

void editor_free_row(erow *row)
{
	free(row->render);
	free(row->chars);
}

void editor_del_row(int at)
{
	if (at < 0 || at >= E.num_rows)
	{
		return;
	}

	editor_free_row(&E.row[at]);
	memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.num_rows - at - 1));
	E.num_rows--;
	E.dirty++;
}

void editor_row_insert_char(erow *row, int at, int c)
{
	if (at < 0 || at > row->size)
	{
		at = row->size;
	}

	row->chars = realloc(row->chars, row->size + 2);
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	editor_update_row(row);
	E.dirty++;
}

void editor_row_append_string(erow *row, char *s, size_t len)
{
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	editor_update_row(row);
	E.dirty++;
}

void editor_row_del_char(erow *row, int at)
{
	if (at < 0 || at >= row->size)
	{
		return;
	}

	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
	row->size--;
	editor_update_row(row);
	E.dirty++;
}

void editor_insert_char(int c)
{
	if (E.cursor_y == E.num_rows)
	{
		editor_insert_row(E.num_rows,"", 0);
	}
	editor_row_insert_char(&E.row[E.cursor_y], E.cursor_x, c);
	E.cursor_x++;
}

void editor_insert_new_line()
{
	if (E.cursor_x == 0)
	{
		editor_insert_row(E.cursor_y, "", 0);
	}
	else
	{
		erow *row = &E.row[E.cursor_y];
		editor_insert_row(E.cursor_y + 1, &row->chars[E.cursor_x], row->size - E.cursor_x);
		row = &E.row[E.cursor_y];
		row->size = E.cursor_x;
		row->chars[row->size] = '\0';
		editor_update_row(row);
	}
	E.cursor_y++;
	E.cursor_x = 0;
}

void editor_del_char()
{
	if (E.cursor_y == E.num_rows)
	{
		return;
	}

	if (E.cursor_x == 0 && E.cursor_y == 0)
	{
		return;
	}

	erow *row = &E.row[E.cursor_y];
	if (E.cursor_x > 0)
	{
		editor_row_del_char(row, E.cursor_x - 1);
		E.cursor_x--;
	}
	else
	{
		E.cursor_x = E.row[E.cursor_y - 1].size;
		editor_row_append_string(&E.row[E.cursor_y - 1], row->chars, row->size);
		editor_del_row(E.cursor_y);
		E.cursor_y--;
	}
}

char *editor_rows_to_string(int *buf_len)
{
	int tot_len = 0;
	int j;
	for (j = 0; j < E.num_rows; j++)
	{
		tot_len += E.row[j].size + 1;
	}
	*buf_len = tot_len;

	char *buf = malloc(tot_len);
	char *p = buf;
	for (j = 0; j < E.num_rows; j++)
	{
		memcpy(p, E.row[j].chars, E.row[j].size);
		p += E.row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}

void editor_open(char *filename)
{
	free(E.filename);
	E.filename = strdup(filename);

	FILE *fp = fopen(filename, "r");
	if (!fp)
	{
		die("fopen");
	}

	char *line = NULL;
	size_t line_cap = 0;
	ssize_t line_len;
	while ((line_len = getline(&line, &line_cap, fp)) != -1)
	{
		while (line_len > 0 && (line[line_len - 1] == '\n' ||
					line[line_len - 1] == '\r'))
		{
			line_len--;
		}
		editor_insert_row(E.num_rows, line, line_len);
	}
	free(line);
	free(fp);
	E.dirty = 0;
}

void editor_save()
{
	if (E.filename == NULL)
	{
		E.filename = editor_prompt("Save as: %s (ESC to cancel)");
		if (E.filename == NULL)
		{
			editor_set_status_message("Save aborted");
			return;
		}
	}

	int len;
	char *buf = editor_rows_to_string(&len);

	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	if (fd != -1)
	{
		if (ftruncate(fd, len) != -1)
		{
			if (write(fd, buf, len) == len)
			{
				close(fd);
				free(buf);
				E.dirty = 0;
				editor_set_status_message("%d bytes written to disk", len);
				return;
			}
		}
		close(fd);
	}
	free(fd);
	editor_set_status_message("Cannot save! I/O error: %s", strerror(errno));
}

struct abuf
{
	char *b;
	int len;
};

#define ABUF_INIT { NULL, 0 }

void ab_append(struct abuf *ab, const char *s, int len)
{
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL)
	{
		return;
	}

	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void ab_free(struct abuf *ab)
{
	free(ab->b);
}

void editor_scroll()
{
	E.render_x = 0;
	if (E.cursor_y < E.num_rows)
	{
		E.render_x = editor_row_cursorx_to_renderx(&E.row[E.cursor_y], E.cursor_x);
	}

	if (E.cursor_y < E.row_off)
	{
		E.row_off = E.cursor_y;
	}

	if (E.cursor_y >= E.row_off + E.screen_rows)
	{
		E.row_off = E.cursor_y - E.screen_rows + 1;
	}

	if (E.render_x < E.col_off)
	{
		E.col_off = E.render_x;
	}

	if (E.render_x >= E.col_off + E.screen_cols)
	{
		E.col_off = E.render_x - E.screen_cols + 1;
	}
}

void editor_draw_rows(struct abuf *ab)
{
	int y;
	for (y = 0; y < E.screen_rows; y++)
	{
		int file_row = y + E.row_off;
		if (file_row >= E.num_rows)
		{
			if (E.num_rows == 0 && y == E.screen_rows / 3)
			{
				char welcome[80];
				int welcome_len = snprintf(welcome, sizeof(welcome),
					"SI-TEXT -- version %s", SITEXT_VERSION);
				if (welcome_len > E.screen_cols)
				{
					welcome_len = E.screen_cols;
				}

				int padding = (E.screen_cols - welcome_len) / 2;
				if (padding)
				{
					ab_append(ab, "~", 1);
					padding--;
				}
				while (padding--)
				{
					ab_append(ab, " ", 1);
				}
				ab_append(ab, welcome, welcome_len);
			}
			else
			{
				ab_append(ab, "~", 1);
			}
		}
		else
		{
			int len = E.row[file_row].rsize - E.col_off;
			if (len < 0)
			{
				len = 0;
			}

			if (len > E.screen_cols)
			{
				len = E.screen_cols;
			}
			ab_append(ab, &E.row[file_row].render[E.col_off], len);
		}

		ab_append(ab, "\x1b[K", 3);
		ab_append(ab, "\r\n", 2);
	}
}

void editor_draw_status_bar(struct abuf *ab)
{
	ab_append(ab, "\x1b[7m", 4);
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
			E.filename ? E.filename : "[No Name]", E.num_rows,
			E.dirty ? "(modified)" : "");
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cursor_y + 1, E.num_rows);
	if (len > E.screen_cols)
	{
		len = E.screen_cols;
	}

	ab_append(ab, status, len);

	while (len < E.screen_cols)
	{
		if (E.screen_cols - len == rlen)
		{
			ab_append(ab, rstatus, rlen);
			break;
		}
		else
		{
			ab_append(ab, " ", 1);
			len++;
		}
	}
	ab_append(ab, "\x1b[m", 3);
	ab_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(struct abuf *ab)
{
	ab_append(ab, "\x1b[K", 3);
	int msglen = strlen(E.statusmsg);
	if (msglen > E.screen_cols)
	{
		msglen = E.screen_cols;
	}

	if (msglen && time(NULL) - E.statusmsg_time > 5)
	{
		ab_append(ab, E.statusmsg, msglen);
	}
}

void editor_refresh_screen()
{
	editor_scroll();
	struct abuf ab = ABUF_INIT;

	ab_append(&ab, "\x1b[?25l", 6);
	ab_append(&ab, "\x1b[H", 3);

	editor_draw_rows(&ab);
	editor_draw_status_bar(&ab);
	editor_draw_message_bar(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cursor_y - E.row_off) + 1,
						  (E.render_x - E.col_off) + 1);
	ab_append(&ab, buf, strlen(buf));

	ab_append(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	ab_free(&ab);
}

void editor_set_status_message(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}

char *editor_prompt(char *prompt)
{
	size_t buf_size = 128;
	char *buf = malloc(buf_size);

	size_t buf_len = 0;
	buf[0] = '\0';

	while (1)
	{
		editor_set_status_message(prompt, buf);
		editor_refresh_screen();

		int c = editor_read_key();
		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
		{
			if (buf_len != 0)
			{
				buf[--buf_len] = '\0';
			}
		}
		else if (c == '\x1b')
		{
			editor_set_status_message("");
			free(buf);
			return NULL;
		}
		else if (c == '\r')
		{
			if (buf_len != 0)
			{
				editor_set_status_message("");
				return buf;
			}
		}
		else if (!iscntrl(c) && c < 128)
		{
			if (buf_len == buf_size - 1)
			{
				buf_size *= 2;
				buf = realloc(buf, buf_size);
			}
			buf[buf_len++] = c;
			buf[buf_len] = '\0';
		}
	}
}

void editor_move_cursor(int key)
{
	erow *row = (E.cursor_y >= E.num_rows) ? NULL : &E.row[E.cursor_y];

	switch (key)
	{
		case ARROW_UP:
			if (E.cursor_y != 0)
			{
				E.cursor_y--;
			}
			break;
		case ARROW_LEFT:
			if (E.cursor_x != 0)
			{
				E.cursor_x--;
			}
			else if (E.cursor_y > 0)
			{
				E.cursor_y--;
				E.cursor_x = E.row[E.cursor_y].size;
			}
			break;
		case ARROW_DOWN:
			if (E.cursor_y < E.num_rows)
			{
				E.cursor_y++;
			}
			break;
		case ARROW_RIGHT:
			if (row && E.cursor_x < row->size)
			{
				E.cursor_x++;
			}
			else if (row && E.cursor_x == row->size)
			{
				E.cursor_y++;
				E.cursor_x = 0;
			}
			break;
	}

	row = (E.cursor_y >= E.num_rows) ? NULL : &E.row[E.cursor_y];
	int row_len = row ? row->size : 0;
	if (E.cursor_x > row_len)
	{
		E.cursor_x = row_len;
	}
}

void editor_process_keypress()
{
	static int quit_times = SITEXT_QUIT_TIMES;

	int c = editor_read_key();

	switch (c)
	{
		case '\r':
			editor_insert_new_line();
			break;

		case CTRL_KEY('q'):
			if (E.dirty && quit_times > 0)
			{
        			editor_set_status_message("Warning! File has unsaved changes. "
			          "Press Ctrl-Q %d more times to quit.", quit_times);
				quit_times--;
				return;
			}
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		case CTRL_KEY('s'):
			editor_save();
			break;

		case HOME_KEY:
			E.cursor_x = 0;
			break;

		case END_KEY:
			if (E.cursor_y < E.num_rows)
			{
				E.cursor_x = E.row[E.cursor_y].size;
			}
			break;

		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
			if (c == DEL_KEY)
			{
				editor_move_cursor(ARROW_RIGHT);
			}
			editor_del_char();
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				if (c == PAGE_UP)
				{
					E.cursor_y = E.row_off;
				}
				else if (c == PAGE_DOWN)
				{
					E.cursor_y = E.row_off + E.screen_rows - 1;
					if (E.cursor_y > E.num_rows)
					{
						E.cursor_y = E.num_rows;
					}
				}

				int times = E.screen_rows;
				while (times--)
				{
					editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
				}
			}
			break;
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editor_move_cursor(c);
			break;
		
		case CTRL_KEY('l'):
		case '\x1b':
			break;

		default:
			editor_insert_char(c);
			break;
	}

	quit_times = SITEXT_QUIT_TIMES;
}

void init_editor()
{
	E.cursor_x = 0;
	E.cursor_y = 0;
	E.render_x = 0;
	E.row_off = 0;
	E.col_off = 0;
	E.num_rows = 0;
	E.row = NULL;
	E.dirty = 0;
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;
	
	if (get_window_size(&E.screen_rows, &E.screen_cols) == -1)
	{
		die("get_window_size");
	}

	E.screen_rows -= 2;
}

int main(int argc, char *argv[])
{
	enable_raw_mode();
	init_editor();
	if (argc >= 2)
	{
		editor_open(argv[1]);
	}

	editor_set_status_message("HELP: Ctrl-S = save | Ctrl-Q = quit");

	while (1)
	{
		editor_refresh_screen();
		editor_process_keypress();
	}
	return 0;
}
