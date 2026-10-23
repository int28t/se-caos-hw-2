//==============================================================================
// Выбор фаз и управление светофорами в отдельном потоке.
//==============================================================================
#include "simulation.h"

#include <inttypes.h>

//------------------------------------------------------------------------------
// Доступ к участнику кольцевой очереди.
static Participant *QueuedLocked(Simulation *s, unsigned i, unsigned offset) {
  Flow *f = &s->flows[i];
  return &f->items[(f->head + offset) % s->cfg->streams[i].capacity];
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
        uint64_t wait = s->nowNs - QueuedLocked(s, i, 0)->arrivedNs;
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
    SimulationEventLocked(s, "Сигнал: поток=%s зеленый=%u",
                          s->cfg->streams[i].name,
                          (s->controller.signals & (UINT32_C(1) << i)) != 0);
}

//------------------------------------------------------------------------------
// Начало зелёной фазы.
static void BeginPhaseLocked(Simulation *s, unsigned phase) {
  Controller *c = &s->controller;
  c->phase = phase;
  c->sinceNs = s->nowNs;
  c->state = GREEN;
  c->signals = s->cfg->phases[phase].mask;
  c->nextFixed = (phase + 1) % s->cfg->phaseCount;
  SimulationEventLocked(s, "Начало фазы: имя=%s", s->cfg->phases[phase].name);
  SignalsOutLocked(s);
  SimulationWakeHeadsLocked(s);
}

//------------------------------------------------------------------------------
// Завершение фазы, освобождение зоны и полный защитный интервал.
static void ControllerNextStateLocked(Simulation *s) {
  Controller *c = &s->controller;
  if (c->state == CLEARANCE) {
    if (s->nowNs - c->sinceNs >= (uint64_t)s->cfg->clearance * s->tickNs) {
      SimulationEventLocked(s, "Интервал окончен: зона свободна");
      BeginPhaseLocked(s, ChoosePhaseLocked(s));
    }
    return;
  }
  uint64_t elapsed = s->nowNs - c->sinceNs;
  if (c->state == GREEN) {
    int urgent = EmergencyPhaseLocked(s);
    bool end = elapsed >= (uint64_t)s->cfg->maxGreen * s->tickNs;
    if (elapsed >= (uint64_t)s->cfg->minGreen * s->tickNs &&
        ((urgent >= 0 && (unsigned)urgent != c->phase) ||
         (s->cfg->strategy == ADAPTIVE && SimulationQueueCountLocked(s) &&
          ChoosePhaseLocked(s) != c->phase)))
      end = true;
    if (end) {
      c->state = DRAIN;
      SimulationEventLocked(s, "Въезд закрыт: фаза=%s пересекают=%u",
                            s->cfg->phases[c->phase].name, s->active);
    }
  }
  // Зелёный для уже движущихся участников сохраняется до выхода последнего.
  if (c->state == DRAIN && !s->active) {
    SimulationEventLocked(s, "Конец фазы: имя=%s длительность=%" PRIu64,
                          s->cfg->phases[c->phase].name, elapsed / s->tickNs);
    c->signals = 0;
    c->state = CLEARANCE;
    c->sinceNs = s->nowNs;
    SignalsOutLocked(s);
    SimulationEventLocked(s, "Интервал: длительность=%u", s->cfg->clearance);
  }
}

//------------------------------------------------------------------------------
static uint64_t ControllerDeadlineLocked(Simulation *s) {
  const Controller *c = &s->controller;
  if (c->state == DRAIN)
    return UINT64_MAX;
  unsigned ticks = s->cfg->maxGreen;
  if (c->state == CLEARANCE)
    ticks = s->cfg->clearance;
  else if (s->nowNs - c->sinceNs < (uint64_t)s->cfg->minGreen * s->tickNs)
    ticks = s->cfg->minGreen;
  uint64_t duration = (uint64_t)ticks * s->tickNs;
  if (c->sinceNs >= UINT64_MAX - duration) {
    SimulationStopLocked(s, STOP_RUNTIME_ERROR, 0);
    return s->nowNs;
  }
  return c->sinceNs + duration;
}

//------------------------------------------------------------------------------
void *ControllerRun(void *argument) {
  Simulation *s = argument;
  SimulationWait(s, s->controllerThread.event, UINT64_MAX);
  SimulationLock(s);
  if (SimulationUpdateLocked(s)) {
    BeginPhaseLocked(s, ChoosePhaseLocked(s));
    for (unsigned i = 0; i < s->cfg->streamCount; ++i)
      if (s->directionThreads[i].worker.started)
        SimulationNotifyLocked(s, &s->directionThreads[i].worker);
  }
  while (SimulationUpdateLocked(s)) {
    ControllerNextStateLocked(s);
    if (!SimulationCheckLocked(s))
      SimulationStopLocked(s, STOP_INVARIANT, 0);
    uint64_t deadline = ControllerDeadlineLocked(s);
    if (s->stopReason != STOP_NONE)
      break;
    SimulationUnlock(s);
    SimulationWait(s, s->controllerThread.event, deadline);
    SimulationLock(s);
  }
  s->controllerThread.finished = true;
  SimulationUnlock(s);
  return NULL;
}
