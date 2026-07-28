#include "terminal.h"

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
  out_mode |=ENABLE_VIRTUAL_TERMINAL_PROCESSING;

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

  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

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
