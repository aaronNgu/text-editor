#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>

/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/
struct editorConfig {
    int screenrows;
    int screencols;
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
char editorReadKey()
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) 
    {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    return c;
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

/*** output ***/
void editorDrawRows() 
{
    int y;
    for (y = 0; y < E.screenrows; y++) 
    {
        write(STDOUT_FILENO, "~\r\n", 3);
    }
}

void editorRefreshScreen()
{
    // write escape sequence(escape + [)
    // \x1b is escape char
    // 2J means clear entire screen
    write(STDOUT_FILENO, "\x1b[2J", 4);
    // H sets Cursor Position to start of terminal 
    write(STDOUT_FILENO, "\x1b[H", 3);

    editorDrawRows();

    write(STDOUT_FILENO, "\x1b[H", 3);
}

/*** input ***/
void editorProcessKeypress() 
{
    char c = editorReadKey();
    switch (c) 
    {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
    }
}

/*** init ***/
void initEditor() 
{
    if (getWindowsSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main()
{
    enableRawMode();
    initEditor();
    while (1)
    {
       editorRefreshScreen();
       editorProcessKeypress();
    }
    return 0;
}
