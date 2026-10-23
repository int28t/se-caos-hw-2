//==============================================================================
// Чтение потоков, фаз и параметров из текстового файла.
//==============================================================================
#include "crossroad.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *kindNames[2] = {"car", "pedestrian"};
const char *directionNames[4] = {"north", "east", "south", "west"};
const char *maneuverNames[4] = {"straight", "right", "left", "walk"};

//------------------------------------------------------------------------------
// Беззнаковое десятичное число без знака, пробелов и переполнения.
static bool ReadNumber(const char *text, uint64_t limit, uint64_t *value) {
  if (!*text)
    return false;
  for (const char *p = text; *p; ++p)
    if (!isdigit((unsigned char)*p))
      return false;
  errno = 0;
  char *end;
  unsigned long long n = strtoull(text, &end, 10);
  if (errno || *end || n > limit)
    return false;
  *value = (uint64_t)n;
  return true;
}

//------------------------------------------------------------------------------
// Поиск текстового обозначения в небольшом массиве.
static int NameIndex(const char *text, const char **names, unsigned count) {
  for (unsigned i = 0; i < count; ++i)
    if (!strcmp(text, names[i]))
      return (int)i;
  return -1;
}

//------------------------------------------------------------------------------
// Имена ограничены буквами ASCII, цифрами, подчёркиванием и дефисом.
static bool ValidName(const char *name) {
  if (!*name || strlen(name) >= NAME_SIZE)
    return false;
  for (const char *p = name; *p; ++p)
    if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-')
      return false;
  return true;
}

//------------------------------------------------------------------------------
// Поиск потока, уже объявленного в конфигурации.
static int StreamIndex(const Config *cfg, const char *name) {
  for (unsigned i = 0; i < cfg->streamCount; ++i)
    if (!strcmp(name, cfg->streams[i].name))
      return (int)i;
  return -1;
}

//------------------------------------------------------------------------------
// Одна строка конфигурации: поток, фаза, конфликт или числовой параметр.
static bool ParseLine(Config *cfg, char **t, unsigned count) {
  if (!strcmp(t[0], "stream") && count == 8) {
    if (cfg->streamCount == MAX_STREAMS || !ValidName(t[1]) ||
        StreamIndex(cfg, t[1]) >= 0)
      return false;
    int kind = NameIndex(t[2], kindNames, 2);
    int direction = NameIndex(t[3], directionNames, 4);
    int maneuver = NameIndex(t[4], maneuverNames, 4);
    uint64_t capacity, crossing, rate;
    if (kind < 0 || direction < 0 || maneuver < 0 ||
        (kind == CAR && maneuver == WALK) ||
        (kind == PEDESTRIAN && maneuver != WALK) ||
        !ReadNumber(t[5], MAX_QUEUE, &capacity) || !capacity ||
        !ReadNumber(t[6], 1000, &crossing) || !crossing ||
        !ReadNumber(t[7], 1000, &rate))
      return false;
    unsigned index = cfg->streamCount++;
    Stream *s = &cfg->streams[index];
    strcpy(s->name, t[1]);
    s->kind = (Kind)kind;
    s->direction = (Direction)direction;
    s->maneuver = (Maneuver)maneuver;
    s->capacity = (unsigned)capacity;
    s->crossingTicks = (unsigned)crossing;
    s->rate = (unsigned)rate;
    // По умолчанию все пары конфликтуют; совместимость задаётся явно.
    cfg->conflicts[index] = UINT32_MAX;
    for (unsigned i = 0; i < index; ++i)
      cfg->conflicts[i] |= UINT32_C(1) << index;
    return true;
  }
  if (!strcmp(t[0], "phase") && count >= 3) {
    if (cfg->phaseCount == MAX_PHASES || !ValidName(t[1]))
      return false;
    for (unsigned i = 0; i < cfg->phaseCount; ++i)
      if (!strcmp(t[1], cfg->phases[i].name))
        return false;
    Phase p = {0};
    strcpy(p.name, t[1]);
    for (unsigned i = 2; i < count; ++i) {
      int index = StreamIndex(cfg, t[i]);
      if (index < 0 || (p.mask & (UINT32_C(1) << (unsigned)index)))
        return false;
      p.mask |= UINT32_C(1) << (unsigned)index;
    }
    cfg->phases[cfg->phaseCount++] = p;
    return true;
  }
  uint64_t n;
  if (!strcmp(t[0], "conflict") && count == 4) {
    int a = StreamIndex(cfg, t[1]), b = StreamIndex(cfg, t[2]);
    if (a < 0 || b < 0 || !ReadNumber(t[3], 1, &n) || (a == b && !n))
      return false;
    uint32_t ba = UINT32_C(1) << (unsigned)a, bb = UINT32_C(1) << (unsigned)b;
    if (n) {
      cfg->conflicts[a] |= bb;
      cfg->conflicts[b] |= ba;
    } else {
      cfg->conflicts[a] &= ~bb;
      cfg->conflicts[b] &= ~ba;
    }
    return true;
  }
  if (count != 2)
    return false;
  if (!strcmp(t[0], "strategy")) {
    if (strcmp(t[1], "fixed") && strcmp(t[1], "adaptive"))
      return false;
    cfg->strategy = !strcmp(t[1], "fixed") ? FIXED : ADAPTIVE;
    return true;
  }
  if (!ReadNumber(t[1], !strcmp(t[0], "seed") ? UINT32_MAX : 1000000000, &n))
    return false;
  if (!strcmp(t[0], "ticks"))
    cfg->ticks = n;
  else if (!strcmp(t[0], "target"))
    cfg->target = n;
  else if (!strcmp(t[0], "seed"))
    cfg->seed = (unsigned)n;
  else if (!strcmp(t[0], "priority") && n <= 1)
    cfg->priority = n != 0;
  else if (!strcmp(t[0], "emergency_rate") && n <= 1000)
    cfg->emergencyRate = (unsigned)n;
  else if (!strcmp(t[0], "delay_ms") && n <= 60000)
    cfg->delayMs = (unsigned)n;
  else if (!strcmp(t[0], "min_green") && n && n <= 10000)
    cfg->minGreen = (unsigned)n;
  else if (!strcmp(t[0], "max_green") && n && n <= 10000)
    cfg->maxGreen = (unsigned)n;
  else if (!strcmp(t[0], "clearance") && n && n <= 10000)
    cfg->clearance = (unsigned)n;
  else
    return false;
  return true;
}

//------------------------------------------------------------------------------
// Чтение файла через open/read/close и разбор строк.
bool ConfigLoad(Config *cfg, const char *path) {
  *cfg = (Config){.minGreen = 4,
                  .maxGreen = 10,
                  .clearance = 2,
                  .emergencyRate = 20,
                  .seed = 39,
                  .ticks = 120,
                  .strategy = ADAPTIVE,
                  .priority = true};
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    perror("Не удалось открыть конфигурацию");
    return false;
  }
  char text[TEXT_SIZE + 1];
  bool ok = ReadText(fd, text, sizeof(text));
  if (close(fd) < 0)
    ok = false;
  if (!ok) {
    dprintf(STDERR_FILENO,
            "Ошибка чтения, превышен размер или найден NUL-байт\n");
    return false;
  }
  unsigned number = 0;
  char *saveLine;
  for (char *line = strtok_r(text, "\n", &saveLine); line;
       line = strtok_r(NULL, "\n", &saveLine)) {
    ++number;
    char *comment = strchr(line, '#');
    if (comment)
      *comment = '\0';
    char *tokens[MAX_STREAMS + 2];
    unsigned count = 0;
    char *saveToken;
    for (char *p = strtok_r(line, " \t\r", &saveToken); p;
         p = strtok_r(NULL, " \t\r", &saveToken)) {
      if (count == MAX_STREAMS + 2) {
        ok = false;
        break;
      }
      tokens[count++] = p;
    }
    if (!ok || (count && !ParseLine(cfg, tokens, count))) {
      dprintf(STDERR_FILENO, "Некорректная конфигурация: непустая строка %u\n",
              number);
      return false;
    }
  }
  return true;
}

//------------------------------------------------------------------------------
// Проверка длительностей, совместимости сигналов и обслуживания каждого потока.
bool ConfigValidate(const Config *cfg) {
  if (!cfg->streamCount || !cfg->phaseCount || cfg->minGreen > cfg->maxGreen) {
    dprintf(STDERR_FILENO, "Нужны потоки, фазы и min_green <= max_green\n");
    return false;
  }
  uint32_t covered = 0;
  for (unsigned p = 0; p < cfg->phaseCount; ++p) {
    uint32_t mask = cfg->phases[p].mask;
    covered |= mask;
    for (unsigned i = 0; i < cfg->streamCount; ++i)
      if ((mask & (UINT32_C(1) << i)) &&
          (cfg->conflicts[i] & mask & ~(UINT32_C(1) << i))) {
        dprintf(STDERR_FILENO, "Конфликтующие потоки в фазе %s\n",
                cfg->phases[p].name);
        return false;
      }
  }
  for (unsigned i = 0; i < cfg->streamCount; ++i)
    if (cfg->streams[i].crossingTicks > cfg->maxGreen ||
        !(covered & (UINT32_C(1) << i))) {
      dprintf(STDERR_FILENO,
              "Проверьте длительность и обслуживающую фазу потока %s\n",
              cfg->streams[i].name);
      return false;
    }
  return true;
}
