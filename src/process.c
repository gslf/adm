#include "process.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#define OUTPUT_LIMIT (8 * 1024 * 1024)
#define PROCESS_TIMEOUT 120000

static uint64_t milliseconds(void) {
#ifdef _WIN32
  return GetTickCount64();
#else
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
}

static int append(process_stream *stream, const char *data, size_t length) {
  if (length > OUTPUT_LIMIT - stream->length)
    return -1;
  size_t needed = stream->length + length + 1;
  if (needed > stream->capacity) {
    size_t capacity = stream->capacity ? stream->capacity * 2 : 4096;
    if (capacity < needed)
      capacity = needed;
    char *more = realloc(stream->data, capacity);
    if (!more)
      return -1;
    stream->data = more;
    stream->capacity = capacity;
  }
  memcpy(stream->data + stream->length, data, length);
  stream->length += length;
  stream->data[stream->length] = '\0';
  return 0;
}

static void close_stream(process_stream *stream) {
  if (!stream->open)
    return;
#ifdef _WIN32
  CloseHandle((HANDLE)stream->handle);
#else
  close((int)stream->handle);
#endif
  stream->open = 0;
}

#ifdef _WIN32
static int make_pipe(process_stream *stream, HANDLE *writer) {
  SECURITY_ATTRIBUTES attributes = {sizeof attributes, NULL, TRUE};
  HANDLE reader;
  if (!CreatePipe(&reader, writer, &attributes, 0))
    return -1;
  if (!SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(reader);
    CloseHandle(*writer);
    *writer = NULL;
    return -1;
  }
  stream->handle = (intptr_t)reader;
  stream->open = 1;
  return 0;
}

static int repeat(process_stream *text, char c, size_t count) {
  while (count--)
    if (append(text, &c, 1) < 0)
      return -1;
  return 0;
}

// Quote argv using Windows' argument rules; no shell ever interprets user input.
static char *command_line(const char *const argv[]) {
  process_stream text = {0};
  for (int i = 0; argv[i]; i++) {
    if (append(&text, i ? " \"" : "\"", i ? 2 : 1) < 0)
      goto fail;
    const char *s = argv[i];
    while (*s) {
      size_t slashes = 0;
      while (*s == '\\') {
        slashes++;
        s++;
      }
      if (repeat(&text, '\\', *s == '"' || !*s ? slashes * 2 : slashes) < 0)
        goto fail;
      if (!*s)
        break;
      if (*s == '"' && append(&text, "\\", 1) < 0)
        goto fail;
      if (append(&text, s++, 1) < 0)
        goto fail;
    }
    if (append(&text, "\"", 1) < 0)
      goto fail;
  }
  return text.data;
fail:
  free(text.data);
  return NULL;
}

static char *environment_block(const char *const env[]) {
  process_stream block = {0};
  char *inherited = GetEnvironmentStringsA();
  if (!inherited)
    return NULL;
  for (char *entry = inherited; *entry; entry += strlen(entry) + 1) {
    int overridden = 0;
    for (int i = 0; env && env[i]; i++) {
      const char *equal = strchr(env[i], '=');
      size_t key = equal ? (size_t)(equal - env[i]) : 0;
      if (key && !_strnicmp(entry, env[i], key) && entry[key] == '=')
        overridden = 1;
    }
    if (!overridden && append(&block, entry, strlen(entry) + 1) < 0)
      goto fail;
  }
  for (int i = 0; env && env[i]; i++)
    if (append(&block, env[i], strlen(env[i]) + 1) < 0)
      goto fail;
  if (append(&block, "", 1) < 0)
    goto fail;
  FreeEnvironmentStringsA(inherited);
  return block.data;
fail:
  FreeEnvironmentStringsA(inherited);
  free(block.data);
  return NULL;
}
#endif

int process_start(child_process *p, const char *const argv[], const char *const env[]) {
  process_dispose(p);
  p->exit_code = -1;
#ifdef _WIN32
  HANDLE out = NULL, err = NULL, input = INVALID_HANDLE_VALUE;
  char *line = command_line(argv), *environment = environment_block(env);
  if (!line || !environment || make_pipe(&p->out, &out) < 0 ||
      make_pipe(&p->err, &err) < 0)
    goto fail;
  SECURITY_ATTRIBUTES attributes = {sizeof attributes, NULL, TRUE};
  input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &attributes, OPEN_EXISTING, 0, NULL);
  if (input == INVALID_HANDLE_VALUE)
    goto fail;
  STARTUPINFOA startup = {0};
  startup.cb = sizeof startup;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input;
  startup.hStdOutput = out;
  startup.hStdError = err;
  PROCESS_INFORMATION info;
  if (!CreateProcessA(NULL, line, NULL, NULL, TRUE,
                      CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                      environment, NULL, &startup, &info))
    goto fail;
  p->handle = (intptr_t)info.hProcess;
  p->id = info.dwProcessId;
  CloseHandle(info.hThread);
  HANDLE job = CreateJobObjectA(NULL, NULL);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                sizeof limits) && AssignProcessToJobObject(job, info.hProcess))
      p->group = (intptr_t)job;
    else
      CloseHandle(job);
  }
  CloseHandle(out);
  CloseHandle(err);
  CloseHandle(input);
  free(line);
  free(environment);
#else
  int out[2], err[2];
  if (pipe(out) < 0)
    return -1;
  if (pipe(err) < 0) {
    close(out[0]);
    close(out[1]);
    return -1;
  }
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    int input = open("/dev/null", O_RDONLY);
    if (input < 0 || dup2(input, 0) < 0 || dup2(out[1], 1) < 0 || dup2(err[1], 2) < 0)
      _exit(127);
    if (input > 2)
      close(input);
    close(out[0]); close(out[1]); close(err[0]); close(err[1]);
    for (int i = 0; env && env[i]; i++) {
      const char *equal = strchr(env[i], '=');
      char key[128];
      size_t length = equal ? (size_t)(equal - env[i]) : 0;
      if (!length || length >= sizeof key)
        _exit(127);
      memcpy(key, env[i], length);
      key[length] = '\0';
      if (setenv(key, equal + 1, 1) < 0)
        _exit(127);
    }
    execvp(argv[0], (char *const *)argv);
    _exit(127);
  }
  close(out[1]);
  close(err[1]);
  if (pid < 0) {
    close(out[0]);
    close(err[0]);
    return -1;
  }
  p->id = p->group = pid;
  p->running = 1;
  p->out.handle = out[0];
  p->err.handle = err[0];
  p->out.open = p->err.open = 1;
  if (fcntl(out[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(err[0], F_SETFL, O_NONBLOCK) < 0) {
    process_dispose(p);
    return -1;
  }
  fcntl(out[0], F_SETFD, FD_CLOEXEC);
  fcntl(err[0], F_SETFD, FD_CLOEXEC);
#endif
  p->running = 1;
  p->started = milliseconds();
  return 0;
#ifdef _WIN32
fail:
  if (out) CloseHandle(out);
  if (err) CloseHandle(err);
  if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
  free(line);
  free(environment);
  process_dispose(p);
  return -1;
#endif
}

static void drain(child_process *p, process_stream *stream) {
  if (!stream->open)
    return;
  for (int i = 0; i < 16; i++) {
    char data[4096];
#ifdef _WIN32
    DWORD available, read;
    if (!PeekNamedPipe((HANDLE)stream->handle, NULL, 0, NULL, &available, NULL)) {
      close_stream(stream);
      break;
    }
    if (!available)
      break;
    if (!ReadFile((HANDLE)stream->handle, data,
                  available < sizeof data ? available : sizeof data, &read, NULL)) {
      close_stream(stream);
      break;
    }
    size_t length = read;
#else
    ssize_t read_count = read((int)stream->handle, data, sizeof data);
    if (read_count < 0 && (errno == EAGAIN || errno == EINTR))
      break;
    if (read_count <= 0) {
      if (read_count < 0)
        p->failed = 1;
      close_stream(stream);
      break;
    }
    size_t length = (size_t)read_count;
#endif
    if (append(stream, data, length) < 0)
      p->failed = 1;
  }
}

void process_cancel(child_process *p) {
  if (p->cancelled || (!p->id && !p->group))
    return;
  p->cancelled = 1;
  p->cancel_started = milliseconds();
#ifdef _WIN32
  if (p->group)
    TerminateJobObject((HANDLE)p->group, 1);
  else if (p->handle)
    TerminateProcess((HANDLE)p->handle, 1);
#else
  if (p->group)
    kill(-(pid_t)p->group, SIGTERM);
  if (p->id)
    kill((pid_t)p->id, SIGTERM);
#endif
}

int process_poll(child_process *p) {
  if (!p->cancelled && p->started && milliseconds() - p->started > PROCESS_TIMEOUT)
    process_cancel(p);
#ifndef _WIN32
  if (p->cancelled && milliseconds() - p->cancel_started > 1000) {
    if (p->group)
      kill(-(pid_t)p->group, SIGKILL);
    if (p->id)
      kill((pid_t)p->id, SIGKILL);
  }
#endif
  drain(p, &p->out);
  drain(p, &p->err);
  if (p->running) {
#ifdef _WIN32
    if (WaitForSingleObject((HANDLE)p->handle, 0) == WAIT_OBJECT_0) {
      DWORD status;
      if (!GetExitCodeProcess((HANDLE)p->handle, &status))
        p->failed = 1;
      else
        p->exit_code = (int)status;
      p->running = 0;
    }
#else
    int status;
    pid_t result = waitpid((pid_t)p->id, &status, WNOHANG);
    if (result > 0) {
      p->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      p->id = 0;
      p->running = 0;
    } else if (result < 0 && errno != EINTR) {
      p->failed = 1;
      p->id = 0;
      p->running = 0;
    }
#endif
  }
  return !p->running && !p->out.open && !p->err.open;
}

void process_dispose(child_process *p) {
  if (p->running) {
    process_cancel(p);
#ifdef _WIN32
    WaitForSingleObject((HANDLE)p->handle, INFINITE);
#else
    uint64_t deadline = milliseconds() + 1000;
    while (p->running && milliseconds() < deadline) {
      process_poll(p);
      if (p->running) {
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
      }
    }
    if (p->running) {
      if (p->group)
        kill(-(pid_t)p->group, SIGKILL);
      if (p->id)
        kill((pid_t)p->id, SIGKILL);
      while (waitpid((pid_t)p->id, NULL, 0) < 0 && errno == EINTR) {}
    }
#endif
  }
  close_stream(&p->out);
  close_stream(&p->err);
#ifdef _WIN32
  if (p->group) CloseHandle((HANDLE)p->group);
  if (p->handle) CloseHandle((HANDLE)p->handle);
#endif
  free(p->out.data);
  free(p->err.data);
  *p = (child_process){0};
}
