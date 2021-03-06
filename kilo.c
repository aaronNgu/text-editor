// better portability
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>

/*** defines ***/
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey  
{
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
};

/*** data ***/
typedef struct erow {
    int size;
    int rsize;
    char * chars;
    // actual characters to draw on the screen, used to handle tabs
    char * render;
} erow;

struct editorConfig {
    // cursor position within file 
    int cx, cy;
    // cursor with tabs consideration 
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    // num rows from user's file
    int numrows;
    erow* row;
    struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/
void die (const char *s) 
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

// Restore from Raw mode
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);
    struct termios raw = E.orig_termios;
    /* Modes to turn off
    ECHO - each key typed to be printed to the terminal
    ICANON - input is sent when user presses Enter
    ISIG - ctrl-c sends SIGINT to process ctrl-z SIGTSTP
    IXON - ctrl-s ctrl-q software control flow 
    IEXTEN - ctrl-v not sure what's this about
    ICRNL - ctrl-m terminal translates (13, '\r') as newlines (10, '\n')
    OPOST - output processing "\n" translates to "\r\n" 
    \r means move pointer to start
    */
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP );
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

    // set time out for read(), returns if doesn't get any input for 
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

// reads one key and returns key press as char
int editorReadKey()
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) 
    {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    
    // handle escape sequence
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[0] == '[') 
        {
            if (seq[1] >= '0' && seq[1] <= '9') 
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                /*
                page up  <esc>[5~ 
                page down <esc>[6~
                home <esc>[1~ <esc>[7~
                end <esc>[4~ <esc>[8~
                */
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
            } else {
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
        } else if (seq[0] == 'O') {
            switch (seq[1]) 
            {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition (int *rows, int *cols) 
{
    char buf[32];
    unsigned int i = 0;

    // n command - query terminal for status information 
    // 6 - cursor position
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) 
    {
        // command above returns \x1b24;80R
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    // put 24;80 into rows and cols 
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

int getWindowsSize(int *rows, int *cols) 
{
    struct winsize ws;
    
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // fallback for when ioctl falls on some systems
        // B moves cursor right and C moves cursor down by 999
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/
int editorRowCxToRx(erow * row, int cx) 
{
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) 
    {
        // how many more right columns 
        if (row->chars[j] == '\t')
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }
    return rx;
}

void editorUpdateRow(erow * row) 
{
    int tabs = 0;
    int j;
    // count tabs
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) 
    {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) 
{
    // stores number of erows
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
}

/*** file i/o ***/

void editorOpen(char *filename) 
{
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    // reads an entire line from fp, store address of buffer to line of size linecap 
    // returns num of char read
    while ((linelen = getline(&line, &linecap, fp)) != -1) 
    {
        // equivalent strip newline and carriage return 
        while (linelen > 0 && 
            (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editorAppendRow(line, linelen);
    }
    free(line);
    fclose(fp);
}

/*** append buffer - store here before writing ***/ 
// used to write out tildes
struct abuf 
{
    char *b;
    int len;
};

// costructor for abuf type 
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) 
{
    // resize memory block pointed by ab->b 
    // internally it 1. extends existing block or 2. allocates new memory and freeing old one
    char *new = realloc(ab->b, ab->len + len);
    
    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab)
{
    free(ab->b);
}

/*** output ***/
void editorScroll()
{
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff) {
        // scroll up 
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        // scroll down - E.rowoff refers to what's at the top of the screen
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff) {
        // scroll left
        E.coloff = E.cx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        // scroll right
        E.coloff = E.cx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab) 
{
    int y;
    for (y = 0; y < E.screenrows; y++) 
    {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            // Print lines not part of file
            if (y == E.screenrows / 3 && E.numrows == 0) {
                char welcome[80];
                // format and store string in buffer
                int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                } 
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }
        abAppend(ab, "\x1b[K", 3);
        if (y < E.screenrows - 1) 
        {
            abAppend(ab, "\r\n",2);
        }
        
    }
}

void editorRefreshScreen()
{
    editorScroll();
    struct abuf ab = ABUF_INIT;
    // hides cursor will its writing tilde
    abAppend(&ab, "\x1b[?25l", 6);
    // write escape sequence(escape + [)
    // \x1b is escape char
    // 2J means clear entire screen
    // OLD  abAppend(&ab, "\x1b[2J", 4);

    // H sets Cursor Position to start of terminal 
    abAppend(&ab, "\x1b[H",3);

    editorDrawRows(&ab);

    char buf[32];
    // move cursor to specfic x and y position
    // not sure why E.cy - E.rowoff
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);
    
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/
void editorMoveCursor(int key) 
{
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    switch(key) {
        case ARROW_LEFT:
            if (E.cx != 0) 
            {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size)
            {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) 
            {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy != E.numrows) 
            {
                E.cy++;
            }
            break;
    }
    // snap cursor back to end of line 
    // when user move past end when they move down
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) 
    {
        E.cx = rowlen;
    }
}

void editorProcessKeypress() 
{
    int c = editorReadKey();
    switch (c) 
    {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = E.screencols - 1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = E.screenrows;
                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP: ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}

/*** init ***/
void initEditor() 
{
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.numrows = 0;
    E.row = NULL;
    E.rowoff = 0;
    E.coloff = 0;
    if (getWindowsSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();
    if (argc >= 2) 
    {
        editorOpen(argv[1]);
    }
    while (1)
    {
       editorRefreshScreen();
       editorProcessKeypress();
    }
    return 0;
}
