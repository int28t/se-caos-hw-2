//==============================================================================
// Загрузка параметров, запуск модели и обработка прерывания.
//==============================================================================
#include "crossroad.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

//------------------------------------------------------------------------------
// Подготовка, моделирование и закрытие журнала.
int main(int argc, char **argv) {
  if (argc < 2 || argc > 3 || !strcmp(argv[1], "--help")) {
    dprintf(STDOUT_FILENO, "Вариант 39. Использование: crossroad CONFIG [LOG]\n"
                           "Журнал по умолчанию: crossroad.log. Ctrl+C "
                           "завершает модель с итогом.\n");
    return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
  }
  struct sigaction action = {.sa_handler = SIG_DFL};
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, NULL) < 0 ||
      sigaction(SIGTERM, &action, NULL) < 0) {
    perror("sigaction");
    return 1;
  }
  action.sa_handler = SIG_IGN;
  if (sigaction(SIGPIPE, &action, NULL) < 0) {
    perror("sigaction");
    return 1;
  }
  Config cfg;
  if (!ConfigLoad(&cfg, argv[1]) || !ConfigValidate(&cfg))
    return 2;
  const char *logPath = argc == 3 ? argv[2] : "crossroad.log";
  struct stat input, log;
  if (stat(argv[1], &input) == 0 && stat(logPath, &log) == 0 &&
      input.st_dev == log.st_dev && input.st_ino == log.st_ino) {
    dprintf(STDERR_FILENO,
            "Конфигурация и журнал должны быть разными файлами\n");
    return 2;
  }
  Output out;
  if (!OutputOpen(&out, logPath))
    return 1;
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  int error = pthread_sigmask(SIG_BLOCK, &signals, NULL);
  if (error) {
    errno = error;
    perror("pthread_sigmask");
    OutputClose(&out);
    return 1;
  }
  int status = SimulationRun(&cfg, &out);
  OutputClose(&out);
  if (out.failed) {
    dprintf(STDERR_FILENO, "Ошибка записи консоли или журнала\n");
    return 1;
  }
  return status;
}
