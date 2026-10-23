//==============================================================================
// Ввод и вывод через файловые дескрипторы.
//==============================================================================
#include "crossroad.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

//------------------------------------------------------------------------------
// Полная запись буфера с повторением коротких и прерванных записей.
static bool WriteAll(int fd, const char *buffer, size_t length) {
  while (length) {
    ssize_t n = write(fd, buffer, length);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    buffer += (size_t)n;
    length -= (size_t)n;
  }
  return true;
}

//------------------------------------------------------------------------------
// Чтение до EOF; один байт оставляется для завершающего нуля.
bool ReadText(int fd, char *buffer, size_t capacity) {
  size_t used = 0;
  for (;;) {
    char extra;
    bool full = used == capacity - 1;
    ssize_t n =
        read(fd, full ? &extra : buffer + used, full ? 1 : capacity - 1 - used);
    if (n < 0 && errno == EINTR && !stopSignal)
      continue;
    if (n < 0 || (full && n > 0))
      return false;
    if (!n)
      break;
    used += (size_t)n;
  }
  if (memchr(buffer, '\0', used))
    return false;
  buffer[used] = '\0';
  return true;
}

//------------------------------------------------------------------------------
// Создание обычного текстового журнала.
bool OutputOpen(Output *out, const char *path) {
  *out = (Output){.logFd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)};
  if (out->logFd < 0) {
    perror("Не удалось открыть журнал");
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
// Ошибка одного канала не мешает сохранить итог в другом исправном канале.
void OutputEvent(Output *out, uint64_t tick, const char *description) {
  char buffer[2048];
  int n = snprintf(buffer, sizeof(buffer), "[t=%" PRIu64 "] %s\n", tick,
                   description);
  if (n < 0 || (size_t)n >= sizeof(buffer)) {
    out->failed = true;
    return;
  }
  if (!out->logFailed && !WriteAll(out->logFd, buffer, (size_t)n))
    out->failed = out->logFailed = true;
  if (!out->consoleFailed && !WriteAll(STDOUT_FILENO, buffer, (size_t)n))
    out->failed = out->consoleFailed = true;
}

//------------------------------------------------------------------------------
// Закрытие файлового дескриптора журнала.
void OutputClose(Output *out) {
  if (close(out->logFd) < 0)
    out->failed = true;
  out->logFd = -1;
}
