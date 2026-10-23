//==============================================================================
// Последовательное моделирование очередей, участников и светофоров.
//==============================================================================
#include "crossroad.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// Состояние одного запуска; очереди имеют фиксированный верхний предел.
typedef struct Simulation {
  const Config *cfg;
  Output *out;
  Flow flows[MAX_STREAMS];
  Controller controller;
  uint64_t tick, nextId, passed;
  unsigned active;
} Simulation;

//------------------------------------------------------------------------------
// Формирование сообщения о событии.
static void EventOut(Simulation *s, const char *format, ...) {
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
static Participant *Queued(Simulation *s, unsigned i, unsigned offset) {
  Flow *f = &s->flows[i];
  return &f->items[(f->head + offset) % s->cfg->streams[i].capacity];
}

//------------------------------------------------------------------------------
// Суммарная длина очередей.
static unsigned QueueCount(const Simulation *s) {
  unsigned count = 0;
  for (unsigned i = 0; i < s->cfg->streamCount; ++i)
    count += s->flows[i].count;
  return count;
}

//------------------------------------------------------------------------------
// Случайное прибытие в каждую полосу или на переход.
static void ParticipantsArrive(Simulation *s) {
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
    EventOut(s, "Прибытие: поток=%s id=%" PRIu64 " экстренный=%u", stream->name,
             p.id, p.emergency);
    if (f->count == stream->capacity) {
      ++f->refused;
      EventOut(s, "Отказ: поток=%s id=%" PRIu64 " длина=%u", stream->name, p.id,
               f->count);
      continue;
    }
    *Queued(s, i, f->count++) = p;
    if (f->count > f->peak)
      f->peak = f->count;
    EventOut(s, "Очередь: поток=%s id=%" PRIu64 " длина=%u", stream->name, p.id,
             f->count);
    if (stream->kind == PEDESTRIAN)
      EventOut(s, "Запрос пешехода: поток=%s id=%" PRIu64, stream->name, p.id);
    if (p.emergency)
      EventOut(s, "Запрос приоритета: поток=%s id=%" PRIu64 " включен=%u",
               stream->name, p.id, s->cfg->priority);
  }
}

//------------------------------------------------------------------------------
// Завершение проезда или перехода и накопление времени ожидания.
static void FinishCrossings(Simulation *s) {
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
    EventOut(s, "Выход: поток=%s id=%" PRIu64 " ожидание=%" PRIu64,
             s->cfg->streams[i].name, f->active.id, wait);
  }
}

//------------------------------------------------------------------------------
// Фаза старейшего экстренного запроса; автомобиль не обгоняет очередь своей
// полосы.
static int EmergencyPhase(Simulation *s) {
  if (!s->cfg->priority)
    return -1;
  int selected = -1;
  uint64_t oldest = UINT64_MAX;
  for (unsigned i = 0; i < s->cfg->streamCount; ++i)
    for (unsigned j = 0; j < s->flows[i].count; ++j) {
      Participant *p = Queued(s, i, j);
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
static unsigned ChoosePhase(Simulation *s) {
  int urgent = EmergencyPhase(s);
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
        uint64_t wait = s->tick - Queued(s, i, 0)->arrived;
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
static void SignalsOut(Simulation *s) {
  for (unsigned i = 0; i < s->cfg->streamCount; ++i)
    EventOut(s, "Сигнал: поток=%s зеленый=%u", s->cfg->streams[i].name,
             (s->controller.signals & (UINT32_C(1) << i)) != 0);
}

//------------------------------------------------------------------------------
// Начало зелёной фазы.
static void BeginPhase(Simulation *s, unsigned phase) {
  Controller *c = &s->controller;
  c->phase = phase;
  c->since = s->tick;
  c->state = GREEN;
  c->signals = s->cfg->phases[phase].mask;
  c->nextFixed = (phase + 1) % s->cfg->phaseCount;
  EventOut(s, "Начало фазы: имя=%s", s->cfg->phases[phase].name);
  SignalsOut(s);
}

//------------------------------------------------------------------------------
// Завершение фазы, освобождение зоны и полный защитный интервал.
static void ControllerNextState(Simulation *s) {
  Controller *c = &s->controller;
  if (c->state == CLEARANCE) {
    if (s->tick - c->since >= s->cfg->clearance) {
      EventOut(s, "Интервал окончен: зона свободна");
      BeginPhase(s, ChoosePhase(s));
    }
    return;
  }
  uint64_t elapsed = s->tick - c->since;
  if (c->state == GREEN) {
    int urgent = EmergencyPhase(s);
    bool end = elapsed >= s->cfg->maxGreen;
    if (elapsed >= s->cfg->minGreen &&
        ((urgent >= 0 && (unsigned)urgent != c->phase) ||
         (s->cfg->strategy == ADAPTIVE && QueueCount(s) &&
          ChoosePhase(s) != c->phase)))
      end = true;
    if (end) {
      c->state = DRAIN;
      EventOut(s, "Въезд закрыт: фаза=%s пересекают=%u",
               s->cfg->phases[c->phase].name, s->active);
    }
  }
  // Зелёный для уже движущихся участников сохраняется до выхода последнего.
  if (c->state == DRAIN && !s->active) {
    EventOut(s, "Конец фазы: имя=%s длительность=%" PRIu64,
             s->cfg->phases[c->phase].name, elapsed);
    c->signals = 0;
    c->state = CLEARANCE;
    c->since = s->tick;
    SignalsOut(s);
    EventOut(s, "Интервал: длительность=%u", s->cfg->clearance);
  }
}

//------------------------------------------------------------------------------
// FIFO-допуск совместимых участников только на зелёный.
static void AdmitParticipants(Simulation *s) {
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
    Participant p = *Queued(s, i, 0);
    EventOut(s, "Разрешение: поток=%s id=%" PRIu64, stream->name, p.id);
    f->head = (f->head + 1) % stream->capacity;
    --f->count;
    f->active = p;
    f->occupied = true;
    f->leaveAt = s->tick + stream->crossingTicks;
    ++s->active;
    occupied |= UINT32_C(1) << i;
    EventOut(s, "Въезд: поток=%s id=%" PRIu64 " длина=%u", stream->name, p.id,
             f->count);
  }
}

//------------------------------------------------------------------------------
// Инварианты остаются включёнными и в Release.
static bool CheckInvariants(Simulation *s) {
  uint32_t occupied = 0;
  unsigned active = 0;
  uint64_t passed = 0;
  for (unsigned i = 0; i < s->cfg->streamCount; ++i) {
    Flow *f = &s->flows[i];
    if (f->count > s->cfg->streams[i].capacity)
      return false;
    uint64_t previous = 0;
    for (unsigned j = 0; j < f->count; ++j) {
      Participant *p = Queued(s, i, j);
      if (!p->id || p->id <= previous || p->arrived > s->tick ||
          (f->occupied && p->id == f->active.id))
        return false;
      previous = p->id;
    }
    if (f->occupied) {
      if (!(s->controller.signals & (UINT32_C(1) << i)) ||
          (s->cfg->conflicts[i] & occupied) || f->leaveAt <= s->tick)
        return false;
      occupied |= UINT32_C(1) << i;
      ++active;
    }
    if ((s->controller.signals & (UINT32_C(1) << i)) &&
        (s->cfg->conflicts[i] & s->controller.signals & ~(UINT32_C(1) << i)))
      return false;
    if (f->arrived !=
        f->refused + f->passed + f->count + (f->occupied ? 1U : 0U))
      return false;
    passed += f->passed;
  }
  return active == s->active && passed == s->passed &&
         (s->controller.state != CLEARANCE ||
          (!active && !s->controller.signals)) &&
         (!s->cfg->target || passed + active <= s->cfg->target);
}

//------------------------------------------------------------------------------
// Параметры и определения потоков позволяют проверить журнал независимо от
// модели.
static void ParametersOut(Simulation *s) {
  const Config *c = s->cfg;
  EventOut(s,
           "Запуск: стратегия=%s минимум=%u максимум=%u интервал=%u seed=%u "
           "цель=%" PRIu64 " период=%" PRIu64 " приоритет=%u",
           c->strategy == FIXED ? "fixed" : "adaptive", c->minGreen,
           c->maxGreen, c->clearance, c->seed, c->target, c->ticks,
           c->priority);
  for (unsigned i = 0; i < c->streamCount; ++i) {
    const Stream *f = &c->streams[i];
    EventOut(s,
             "Поток: имя=%s тип=%s направление=%s маневр=%s емкость=%u "
             "пересечение=%u"
             " интенсивность=%u конфликты=%" PRIu32,
             f->name, kindNames[f->kind], directionNames[f->direction],
             maneuverNames[f->maneuver], f->capacity, f->crossingTicks, f->rate,
             c->conflicts[i]);
  }
  for (unsigned p = 0; p < c->phaseCount; ++p)
    EventOut(s, "Фаза: имя=%s маска=%" PRIu32, c->phases[p].name,
             c->phases[p].mask);
}

//------------------------------------------------------------------------------
// Итог учитывает прошедших, отказы, очереди и незавершённые пересечения.
static void StatisticsOut(Simulation *s, const char *reason) {
  uint64_t arrived = 0, refused = 0, waitSum = 0, waitMax = 0;
  EventOut(s, "Завершение: причина=%s сигнал=%d", reason, (int)stopSignal);
  for (unsigned i = 0; i < s->cfg->streamCount; ++i) {
    Flow *f = &s->flows[i];
    arrived += f->arrived;
    refused += f->refused;
    waitSum += f->waitSum;
    if (f->waitMax > waitMax)
      waitMax = f->waitMax;
    EventOut(s,
             "Итог потока: имя=%s прибыло=%" PRIu64 " прошло=%" PRIu64
             " отказов=%" PRIu64
             " ожидают=%u в_зоне=%u пик=%u ожидание=%" PRIu64
             " максимум=%" PRIu64,
             s->cfg->streams[i].name, f->arrived, f->passed, f->refused,
             f->count, f->occupied, f->peak, f->waitSum, f->waitMax);
  }
  EventOut(s,
           "ИТОГО: прибыло=%" PRIu64 " прошло=%" PRIu64 " отказов=%" PRIu64
           " ожидают=%u в_зоне=%u ожидание=%" PRIu64
           " среднее=%.2f максимум=%" PRIu64,
           arrived, s->passed, refused, QueueCount(s), s->active, waitSum,
           s->passed ? (double)waitSum / (double)s->passed : 0.0, waitMax);
}

//------------------------------------------------------------------------------
// Один цикл имитирует все потоки; выходы на конечной границе периода
// учитываются.
int SimulationRun(const Config *cfg, Output *out) {
  Simulation s = {.cfg = cfg, .out = out};
  srand(cfg->seed);
  ParametersOut(&s);
  const char *reason;
  int status = 0;
  bool arrivals = false, started = false;
  for (unsigned i = 0; i < cfg->streamCount; ++i)
    if (cfg->streams[i].rate)
      arrivals = true;
  for (;;) {
    FinishCrossings(&s);
    if (!CheckInvariants(&s)) {
      reason = "нарушен_инвариант";
      status = 1;
      break;
    }
    if (stopSignal) {
      reason = "прерывание";
      status = 128 + (int)stopSignal;
      break;
    }
    if (out->failed) {
      reason = "ошибка_вывода";
      status = 1;
      break;
    }
    if (cfg->target && s.passed >= cfg->target) {
      reason = "цель";
      break;
    }
    if (cfg->ticks && s.tick >= cfg->ticks) {
      reason = "период";
      break;
    }
    if (!arrivals && !s.active && !QueueCount(&s)) {
      reason = cfg->target ? "цель_недостижима" : "исчерпан";
      status = cfg->target ? 3 : 0;
      break;
    }
    ParticipantsArrive(&s);
    if (!started) {
      BeginPhase(&s, ChoosePhase(&s));
      started = true;
    } else
      ControllerNextState(&s);
    AdmitParticipants(&s);
    if (!CheckInvariants(&s)) {
      reason = "нарушен_инвариант";
      status = 1;
      break;
    }
    EventOut(&s, "Состояние: ожидают=%u пересекают=%u", QueueCount(&s),
             s.active);
    if (cfg->delayMs && !out->failed && !stopSignal) {
      struct timespec delay = {.tv_sec = (time_t)(cfg->delayMs / 1000),
                               .tv_nsec =
                                   (long)(cfg->delayMs % 1000) * 1000000L};
      while (nanosleep(&delay, &delay) < 0 && errno == EINTR && !stopSignal) {
      }
    }
    ++s.tick;
  }
  StatisticsOut(&s, reason);
  return out->failed ? 1 : status;
}
