//==============================================================================
// Последовательное моделирование очередей, участников и светофоров.
//==============================================================================
#include "simulation.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

//------------------------------------------------------------------------------
// Формирование сообщения о событии.
static void EventOutLocked(Simulation *s, const char *format, ...) {
  char text[1800];
  va_list args;
  va_start(args, format);
  int n = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (n < 0 || (size_t)n >= sizeof(text)) {
    s->out->failed = true;
    return;
  }
  OutputEvent(s->out, s->tick, text);
}

//------------------------------------------------------------------------------
// Доступ к участнику кольцевой очереди.
static Participant *QueuedLocked(Simulation *s, unsigned i, unsigned offset) {
  Flow *f = &s->flows[i];
  return &f->items[(f->head + offset) % s->cfg->streams[i].capacity];
}

//------------------------------------------------------------------------------
// Суммарная длина очередей.
static unsigned QueueCountLocked(const Simulation *s) {
  unsigned count = 0;
  for (unsigned i = 0; i < s->cfg->streamCount; ++i)
    count += s->flows[i].count;
  return count;
}

//------------------------------------------------------------------------------
// Случайное прибытие в каждую полосу или на переход.
static void ParticipantsArriveLocked(Simulation *s) {
  for (unsigned i = 0; i < s->cfg->streamCount; ++i) {
    const Stream *stream = &s->cfg->streams[i];
    if (!stream->rate || (unsigned)rand() % 1000 >= stream->rate)
      continue;
    Flow *f = &s->flows[i];
    Participant p = {.id = ++s->nextId,
                     .arrived = s->tick,
                     .emergency =
                         stream->kind == CAR &&
                         (unsigned)rand() % 1000 < s->cfg->emergencyRate};
    ++f->arrived;
    EventOutLocked(s, "Прибытие: поток=%s id=%" PRIu64 " экстренный=%u",
                   stream->name, p.id, p.emergency);
    if (f->count == stream->capacity) {
      ++f->refused;
      EventOutLocked(s, "Отказ: поток=%s id=%" PRIu64 " длина=%u", stream->name,
                     p.id, f->count);
      continue;
    }
    *QueuedLocked(s, i, f->count++) = p;
    if (f->count > f->peak)
      f->peak = f->count;
    EventOutLocked(s, "Очередь: поток=%s id=%" PRIu64 " длина=%u", stream->name,
                   p.id, f->count);
    if (stream->kind == PEDESTRIAN)
      EventOutLocked(s, "Запрос пешехода: поток=%s id=%" PRIu64, stream->name,
                     p.id);
    if (p.emergency)
      EventOutLocked(s, "Запрос приоритета: поток=%s id=%" PRIu64 " включен=%u",
                     stream->name, p.id, s->cfg->priority);
  }
}

//------------------------------------------------------------------------------
// Завершение проезда или перехода и накопление времени ожидания.
static void FinishCrossingsLocked(Simulation *s) {
  for (unsigned i = 0; i < s->cfg->streamCount; ++i) {
    Flow *f = &s->flows[i];
    if (!f->occupied || f->leaveAt > s->tick)
      continue;
    f->occupied = false;
    --s->active;
    ++s->passed;
    ++f->passed;
    uint64_t wait =
        f->leaveAt - s->cfg->streams[i].crossingTicks - f->active.arrived;
    f->waitSum += wait;
    if (wait > f->waitMax)
      f->waitMax = wait;
    EventOutLocked(s, "Выход: поток=%s id=%" PRIu64 " ожидание=%" PRIu64,
                   s->cfg->streams[i].name, f->active.id, wait);
  }
}

//------------------------------------------------------------------------------
// Фаза старейшего экстренного запроса; автомобиль не обгоняет очередь своей
// полосы.
static int EmergencyPhaseLocked(Simulation *s) {
  if (!s->cfg->priority)
    return -1;
  int selected = -1;
  uint64_t oldest = UINT64_MAX;
  for (unsigned i = 0; i < s->cfg->streamCount; ++i)
    for (unsigned j = 0; j < s->flows[i].count; ++j) {
      Participant *p = QueuedLocked(s, i, j);
      if (p->emergency && p->id < oldest) {
        oldest = p->id;
        selected = (int)i;
      }
    }
  if (selected < 0)
    return -1;
  uint32_t bit = UINT32_C(1) << (unsigned)selected;
  if (s->controller.signals & bit)
    return (int)s->controller.phase;
  for (unsigned p = 0; p < s->cfg->phaseCount; ++p)
    if (s->cfg->phases[p].mask & bit)
      return (int)p;
  return -1;
}

//------------------------------------------------------------------------------
// Фиксированный цикл или выбор по возрасту ожидания, затем длине очереди.
static unsigned ChoosePhaseLocked(Simulation *s) {
  int urgent = EmergencyPhaseLocked(s);
  if (urgent >= 0)
    return (unsigned)urgent;
  unsigned next = s->controller.nextFixed, best = next;
  if (s->cfg->strategy == FIXED)
    return next;
  uint64_t bestAge = 0;
  unsigned bestLength = 0;
  for (unsigned k = 0; k < s->cfg->phaseCount; ++k) {
    unsigned p = (next + k) % s->cfg->phaseCount, length = 0;
    uint64_t age = 0;
    for (unsigned i = 0; i < s->cfg->streamCount; ++i)
      if ((s->cfg->phases[p].mask & (UINT32_C(1) << i)) && s->flows[i].count) {
        uint64_t wait = s->tick - QueuedLocked(s, i, 0)->arrived;
        if (wait > age)
          age = wait;
        length += s->flows[i].count;
      }
    if (length && (!bestLength || age > bestAge ||
                   (age == bestAge && length > bestLength))) {
      best = p;
      bestAge = age;
      bestLength = length;
    }
  }
  return best;
}

//------------------------------------------------------------------------------
// Текущее состояние сигналов каждого потока.
static void SignalsOutLocked(Simulation *s) {
  for (unsigned i = 0; i < s->cfg->streamCount; ++i)
    EventOutLocked(s, "Сигнал: поток=%s зеленый=%u", s->cfg->streams[i].name,
                   (s->controller.signals & (UINT32_C(1) << i)) != 0);
}

//------------------------------------------------------------------------------
// Начало зелёной фазы.
static void BeginPhaseLocked(Simulation *s, unsigned phase) {
  Controller *c = &s->controller;
  c->phase = phase;
  c->since = s->tick;
  c->state = GREEN;
  c->signals = s->cfg->phases[phase].mask;
  c->nextFixed = (phase + 1) % s->cfg->phaseCount;
  EventOutLocked(s, "Начало фазы: имя=%s", s->cfg->phases[phase].name);
  SignalsOutLocked(s);
}

//------------------------------------------------------------------------------
// Завершение фазы, освобождение зоны и полный защитный интервал.
static void ControllerNextStateLocked(Simulation *s) {
  Controller *c = &s->controller;
  if (c->state == CLEARANCE) {
    if (s->tick - c->since >= s->cfg->clearance) {
      EventOutLocked(s, "Интервал окончен: зона свободна");
      BeginPhaseLocked(s, ChoosePhaseLocked(s));
    }
    return;
  }
  uint64_t elapsed = s->tick - c->since;
  if (c->state == GREEN) {
    int urgent = EmergencyPhaseLocked(s);
    bool end = elapsed >= s->cfg->maxGreen;
    if (elapsed >= s->cfg->minGreen &&
        ((urgent >= 0 && (unsigned)urgent != c->phase) ||
         (s->cfg->strategy == ADAPTIVE && QueueCountLocked(s) &&
          ChoosePhaseLocked(s) != c->phase)))
      end = true;
    if (end) {
      c->state = DRAIN;
      EventOutLocked(s, "Въезд закрыт: фаза=%s пересекают=%u",
                     s->cfg->phases[c->phase].name, s->active);
    }
  }
  // Зелёный для уже движущихся участников сохраняется до выхода последнего.
  if (c->state == DRAIN && !s->active) {
    EventOutLocked(s, "Конец фазы: имя=%s длительность=%" PRIu64,
                   s->cfg->phases[c->phase].name, elapsed);
    c->signals = 0;
    c->state = CLEARANCE;
    c->since = s->tick;
    SignalsOutLocked(s);
    EventOutLocked(s, "Интервал: длительность=%u", s->cfg->clearance);
  }
}

//------------------------------------------------------------------------------
// FIFO-допуск совместимых участников только на зелёный.
static void AdmitParticipantsLocked(Simulation *s) {
  Controller *c = &s->controller;
  if (c->state != GREEN)
    return;
  uint32_t occupied = 0;
  for (unsigned i = 0; i < s->cfg->streamCount; ++i)
    if (s->flows[i].occupied)
      occupied |= UINT32_C(1) << i;
  for (unsigned i = 0; i < s->cfg->streamCount; ++i) {
    Flow *f = &s->flows[i];
    const Stream *stream = &s->cfg->streams[i];
    if (!f->count || f->occupied || !(c->signals & (UINT32_C(1) << i)))
      continue;
    if (s->cfg->target && s->passed + s->active >= s->cfg->target)
      break;
    if (s->tick - c->since + stream->crossingTicks > s->cfg->maxGreen ||
        (s->cfg->conflicts[i] & occupied))
      continue;
    Participant p = *QueuedLocked(s, i, 0);
    EventOutLocked(s, "Разрешение: поток=%s id=%" PRIu64, stream->name, p.id);
    f->head = (f->head + 1) % stream->capacity;
    --f->count;
    f->active = p;
    f->occupied = true;
    f->leaveAt = s->tick + stream->crossingTicks;
    ++s->active;
    occupied |= UINT32_C(1) << i;
    EventOutLocked(s, "Въезд: поток=%s id=%" PRIu64 " длина=%u", stream->name,
                   p.id, f->count);
  }
}

//------------------------------------------------------------------------------
// Параметры и определения потоков позволяют проверить журнал независимо от
// модели.
static void ParametersOutLocked(Simulation *s) {
  const Config *c = s->cfg;
  EventOutLocked(
      s,
      "Запуск: стратегия=%s минимум=%u максимум=%u интервал=%u seed=%u "
      "цель=%" PRIu64 " период=%" PRIu64 " приоритет=%u",
      c->strategy == FIXED ? "fixed" : "adaptive", c->minGreen, c->maxGreen,
      c->clearance, c->seed, c->target, c->ticks, c->priority);
  for (unsigned i = 0; i < c->streamCount; ++i) {
    const Stream *f = &c->streams[i];
    EventOutLocked(s,
                   "Поток: имя=%s тип=%s направление=%s маневр=%s емкость=%u "
                   "пересечение=%u"
                   " интенсивность=%u конфликты=%" PRIu32,
                   f->name, kindNames[f->kind], directionNames[f->direction],
                   maneuverNames[f->maneuver], f->capacity, f->crossingTicks,
                   f->rate, c->conflicts[i]);
  }
  for (unsigned p = 0; p < c->phaseCount; ++p)
    EventOutLocked(s, "Фаза: имя=%s маска=%" PRIu32, c->phases[p].name,
                   c->phases[p].mask);
}

//------------------------------------------------------------------------------
// Итог учитывает прошедших, отказы, очереди и незавершённые пересечения.
static void StatisticsOutLocked(Simulation *s, const char *reason) {
  uint64_t arrived = 0, refused = 0, waitSum = 0, waitMax = 0;
  EventOutLocked(s, "Завершение: причина=%s сигнал=%d", reason,
                 s->signalNumber);
  for (unsigned i = 0; i < s->cfg->streamCount; ++i) {
    Flow *f = &s->flows[i];
    arrived += f->arrived;
    refused += f->refused;
    waitSum += f->waitSum;
    if (f->waitMax > waitMax)
      waitMax = f->waitMax;
    EventOutLocked(s,
                   "Итог потока: имя=%s прибыло=%" PRIu64 " прошло=%" PRIu64
                   " отказов=%" PRIu64
                   " ожидают=%u в_зоне=%u пик=%u ожидание=%" PRIu64
                   " максимум=%" PRIu64,
                   s->cfg->streams[i].name, f->arrived, f->passed, f->refused,
                   f->count, f->occupied, f->peak, f->waitSum, f->waitMax);
  }
  EventOutLocked(s,
                 "ИТОГО: прибыло=%" PRIu64 " прошло=%" PRIu64
                 " отказов=%" PRIu64 " ожидают=%u в_зоне=%u ожидание=%" PRIu64
                 " среднее=%.2f максимум=%" PRIu64,
                 arrived, s->passed, refused, QueueCountLocked(s), s->active,
                 waitSum, s->passed ? (double)waitSum / (double)s->passed : 0.0,
                 waitMax);
}

//------------------------------------------------------------------------------
// Один цикл имитирует все потоки; выходы на конечной границе периода
// учитываются.
int SimulationRun(const Config *cfg, Output *out) {
  Simulation s;
  if (!SimulationInit(&s, cfg, out)) {
    perror("Не удалось подготовить состояние модели");
    return 1;
  }
  srand(cfg->seed);
  SimulationLock(&s);
  ParametersOutLocked(&s);
  bool arrivals = false, started = false;
  for (unsigned i = 0; i < cfg->streamCount; ++i)
    if (cfg->streams[i].rate)
      arrivals = true;
  for (;;) {
    if (s.stopReason != STOP_NONE)
      break;
    FinishCrossingsLocked(&s);
    if (!SimulationCheckLocked(&s))
      SimulationStopLocked(&s, STOP_INVARIANT, 0);
    else if (stopSignal)
      SimulationStopLocked(&s, STOP_SIGNAL, (int)stopSignal);
    else if (out->failed)
      SimulationStopLocked(&s, STOP_OUTPUT_ERROR, 0);
    else if (cfg->target && s.passed >= cfg->target)
      SimulationStopLocked(&s, STOP_TARGET, 0);
    else if (cfg->ticks && s.tick >= cfg->ticks)
      SimulationStopLocked(&s, STOP_PERIOD, 0);
    else if (!arrivals && !s.active && !QueueCountLocked(&s))
      SimulationStopLocked(&s, cfg->target ? STOP_UNREACHABLE : STOP_EXHAUSTED,
                           0);
    if (s.stopReason != STOP_NONE)
      break;
    ParticipantsArriveLocked(&s);
    if (!started) {
      BeginPhaseLocked(&s, ChoosePhaseLocked(&s));
      started = true;
    } else
      ControllerNextStateLocked(&s);
    AdmitParticipantsLocked(&s);
    if (!SimulationCheckLocked(&s)) {
      SimulationStopLocked(&s, STOP_INVARIANT, 0);
      break;
    }
    EventOutLocked(&s, "Состояние: ожидают=%u пересекают=%u",
                   QueueCountLocked(&s), s.active);
    bool wait = cfg->delayMs && !out->failed && !stopSignal;
    SimulationUnlock(&s);
    if (wait) {
      struct timespec delay = {.tv_sec = (time_t)(cfg->delayMs / 1000),
                               .tv_nsec =
                                   (long)(cfg->delayMs % 1000) * 1000000L};
      while (nanosleep(&delay, &delay) < 0 && errno == EINTR && !stopSignal) {
      }
    }
    SimulationLock(&s);
    ++s.tick;
  }
  StatisticsOutLocked(&s, SimulationStopName(s.stopReason));
  int status = out->failed ? 1 : s.status;
  SimulationUnlock(&s);
  if (!SimulationDestroy(&s)) {
    perror("Не удалось освободить состояние модели");
    return 1;
  }
  return status;
}
