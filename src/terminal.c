#include "terminal.h"
#include "runtime.h"

#include <stdio.h>
#include<stdlib.h>

///////////////////////////////////
// WINDOWS
#ifdef _WIN32

#include <windows.h>

static DWORD orig_in_mode;
static DWORD orig_out_mode;
static int saved = 0;
static termsize last_size = {0,0};

int init_raw (void){
  HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);

  if (hin == INVALID_HANDLE_VALUE || hout == INVALID_HANDLE_VALUE)
    return -1;

  if(!GetConsoleMode(hin, &orig_in_mode))
    return -1;

  if(!GetConsoleMode(hout, &orig_out_mode))
    return -1;

  saved = 1;
  atexit(restore);

  DWORD in_mode = orig_in_mode;

  // RAW MODE
  in_mode &= ~ENABLE_LINE_INPUT;
  in_mode &= ~ENABLE_ECHO_INPUT;
  in_mode &= ~ENABLE_PROCESSED_INPUT;
  // Special keys as ANSI escape sequences
  in_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;

  if(!SetConsoleMode(hin, in_mode))
    return -1;

  DWORD out_mode = orig_out_mode;

  // ANSI sequences
  out_mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
  // Every row is positioned explicitly. Writing the bottom-right cell must
  // not wrap immediately and scroll the entire console on each frame.
  out_mode &= ~ENABLE_WRAP_AT_EOL_OUTPUT;

  if(!SetConsoleMode(hout, out_mode))
    return -1;

  last_size = get_size();
  return 0;
}

void restore(){
  if (!saved)
    return;

  SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), orig_in_mode);
  SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), orig_out_mode);

  saved = 0;
}

termsize get_size(void){
  termsize s = {0,0};
  CONSOLE_SCREEN_BUFFER_INFO info;

  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)){
    s.cols = info.srWindow.Right - info.srWindow.Left + 1;
    s.rows = info.srWindow.Bottom - info.srWindow.Top +1;
  }

  return s;
}

int term_resized(void){
  termsize now = get_size();

  if(now.rows == 0)
    return 0;

  if(now.rows != last_size.rows || now.cols != last_size.cols){
    last_size = now;
    return 1;
  }

  return 0;
}

///////////////////////////////////
// POSIX
#else

#include <termios.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>

static struct termios orig_termios;
static int saved = 0;

static volatile sig_atomic_t resized = 0;

static void sigwinch_handler(int sig){
  (void) sig; // UNUSED WARNING BYPASS
  resized = 1;
}  

int init_raw (void){

  // Save status
  if (tcgetattr(STDIN_FILENO,&orig_termios) == -1)
    return -1;
  saved = 1;

  atexit(restore);

  struct termios raw = orig_termios;
  raw.c_iflag &= ~ICRNL;
  raw.c_iflag &= ~IXON;

  raw.c_oflag &= ~OPOST;

  raw.c_lflag &= ~ECHO;
  raw.c_lflag &= ~ICANON;
  raw.c_lflag &= ~IEXTEN;
  raw.c_lflag &= ~ISIG;

  // Reads never block: term_wait decides how long to wait for input.
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    return -1;

  // RESIZE HANDLER
  struct sigaction sa;
  sa.sa_handler = sigwinch_handler;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGWINCH, &sa, NULL);

  return 0;
}

void restore(void){
  if (!saved)
    return;

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
  saved = 0;
}

termsize get_size(void){
  termsize s = {0,0};

  struct winsize ws;
  if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 && ws.ws_col !=0 ){
    s.rows = ws.ws_row;
    s.cols = ws.ws_col;
  }

  return s;
}

int term_resized(void){
  if (resized){
    resized = 0;
    return 1;
  }

  return 0;
}

#endif

#ifdef _WIN32
#include <io.h>

// Modifier presses reach the console as key events but never as bytes.
static int modifier_only(const KEY_EVENT_RECORD *key) {
  switch (key->wVirtualKeyCode) {
  case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_CAPITAL:
  case VK_LWIN: case VK_RWIN: case VK_NUMLOCK: case VK_SCROLL:
    return 1;
  default:
    return 0;
  }
}
#endif

// Input already read from the terminal but not yet decoded into keys. A key
// arrives as one burst, so a whole escape sequence is usually here at once.
static unsigned char pending[512];
static int pending_start, pending_end;

int term_wait(int milliseconds) {
  if (pending_start < pending_end)
    return 1;
#ifdef _WIN32
  HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  DWORD start = GetTickCount();
  for (;;) {
    DWORD elapsed = GetTickCount() - start;
    DWORD remaining = elapsed < (DWORD)milliseconds ? (DWORD)milliseconds - elapsed : 0;
    if (WaitForSingleObject(input, remaining) != WAIT_OBJECT_0)
      return 0;
    INPUT_RECORD record;
    DWORD count;
    if (!PeekConsoleInput(input, &record, 1, &count) || !count) return 0;
    if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown &&
        !modifier_only(&record.Event.KeyEvent))
      return 1;
    // Resize/focus/mouse events and modifier presses do not yield bytes
    // through the CRT reader, which would otherwise block on them.
    ReadConsoleInput(input, &record, 1, &count);
  }
#else
  struct pollfd input = {STDIN_FILENO, POLLIN, 0};
  return poll(&input, 1, milliseconds) > 0 && (input.revents & POLLIN);
#endif
}

int term_read(int milliseconds) {
  if (pending_start == pending_end) {
    if (!term_wait(milliseconds))
      return -1;
#ifdef _WIN32
    int n = _read(0, pending, 1);
#else
    int n = (int)read(STDIN_FILENO, pending, sizeof pending);
#endif
    pending_start = 0;
    pending_end = n > 0 ? n : 0;
    if (!pending_end)
      return -1;
  }
  return pending[pending_start++];
}

void term_unread(void) {
  if (pending_start > 0)
    pending_start--;
}

// Out of line, so that key decoding does not pull in <windows.h>.
unsigned long long term_milliseconds(void) {
  return adm_milliseconds();
}
