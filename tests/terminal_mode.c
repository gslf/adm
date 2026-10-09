#include "terminal.h"
#include <assert.h>
#include <windows.h>

int main(void) {
  // The test runner supplies a hidden console, independent of redirected stdout.
  HANDLE input = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
  HANDLE output = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, 0, NULL);
  assert(input != INVALID_HANDLE_VALUE && output != INVALID_HANDLE_VALUE);
  HANDLE saved_input = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE saved_output = GetStdHandle(STD_OUTPUT_HANDLE);
  assert(SetStdHandle(STD_INPUT_HANDLE, input));
  assert(SetStdHandle(STD_OUTPUT_HANDLE, output));
  DWORD in_mode, out_mode, current;
  assert(GetConsoleMode(input, &in_mode));
  assert(GetConsoleMode(output, &out_mode));
  out_mode |= ENABLE_WRAP_AT_EOL_OUTPUT;
  assert(SetConsoleMode(output, out_mode));

  assert(init_raw() == 0);
  assert(GetConsoleMode(output, &current));
  assert(current & ENABLE_PROCESSED_OUTPUT);
  assert(current & ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  assert(!(current & ENABLE_WRAP_AT_EOL_OUTPUT));
  // A full-width bottom row must not scroll the top row out of the screen.
  CONSOLE_SCREEN_BUFFER_INFO info;
  assert(GetConsoleScreenBufferInfo(output, &info));
  COORD top = {0, 0}, bottom = {0, (SHORT)(info.dwSize.Y - 1)};
  DWORD written;
  assert(WriteConsoleOutputCharacterA(output, "X", 1, top, &written));
  assert(SetConsoleCursorPosition(output, bottom));
  assert(FillConsoleOutputCharacterA(output, ' ', info.dwSize.X, bottom, &written));
  // Use the sequential VT output path for the last cell, as the renderer does.
  bottom.X = info.dwSize.X - 1;
  assert(SetConsoleCursorPosition(output, bottom));
  assert(WriteConsoleA(output, " ", 1, &written, NULL));
  char marker;
  assert(ReadConsoleOutputCharacterA(output, &marker, 1, top, &written));
  assert(marker == 'X');

  restore();
  assert(GetConsoleMode(input, &current) && current == in_mode);
  assert(GetConsoleMode(output, &current) && current == out_mode);
  restore();
  assert(SetStdHandle(STD_INPUT_HANDLE, saved_input));
  assert(SetStdHandle(STD_OUTPUT_HANDLE, saved_output));
  CloseHandle(input);
  CloseHandle(output);
  return 0;
}
