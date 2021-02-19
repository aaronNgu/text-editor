#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/
struct termios orig_termios;

/*** terminal ***/
void die (const char *s) 
{
    perror(s);
    exit(1);
}

// Restore from Raw mode
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    // from stdlib
    atexit(disableRawMode);
    struct termios raw = orig_termios;
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

/*** input ***/
void editorProcessKeypress() 
{
    char c = editorReadKey();
    switch (c) 
    {
        case CTRL_KEY('q'):
            exit(0);
            break;
    }
}

/*** init ***/
int main()
{
    enableRawMode();
    while (1)
    {
       editorProcessKeypress();
    }
    return 0;
}
