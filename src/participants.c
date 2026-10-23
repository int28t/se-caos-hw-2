//==============================================================================
// Потоки направлений и участников перекрёстка.
//==============================================================================
#include "simulation.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

//------------------------------------------------------------------------------
// Решение привязано к направлению и такту, а не к порядку исполнения потоков.
unsigned ParticipantRandom(unsigned seed, unsigned stream, uint64_t tick,
                           bool emergency) {
  uint64_t value = seed ^ ((uint64_t)stream << 32) ^
                   (tick * UINT64_C(0x9e3779b97f4a7c15)) ^
                   (emergency ? UINT64_C(0xd1b54a32d192ed03) : 0);
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return (unsigned)((value ^ (value >> 31)) % 1000);
}

//------------------------------------------------------------------------------
void SimulationNotifyLocked(Simulation *s, Worker *worker) {
  if (sem_post(worker->event) < 0)
    SimulationStopLocked(s, STOP_RUNTIME_ERROR, 0);
}

//------------------------------------------------------------------------------
void SimulationWakeHeadsLocked(Simulation *s) {
  if (s->stopReason != STOP_NONE)
    return;
  for (unsigned i = 0; i < s->cfg->streamCount; ++i) {
    const Flow *flow = &s->flows[i];
    if (!flow->count)
      continue;
    const DirectionWorker *direction = &s->directionThreads[i];
    for (unsigned j = 0; j < direction->participantLimit; ++j) {
      ParticipantWorker *p = &s->participants[direction->firstParticipant + j];
      if (p->worker.started && p->participantId == flow->items[flow->head].id) {
        SimulationNotifyLocked(s, &p->worker);
        break;
      }
    }
  }
}

//------------------------------------------------------------------------------
static bool CanEnterLocked(const Simulation *s, const ParticipantWorker *p) {
  const Flow *flow = &s->flows[p->stream];
  const Controller *controller = &s->controller;
  if (s->stopReason != STOP_NONE || controller->state != GREEN ||
      !(controller->signals & (UINT32_C(1) << p->stream)) ||
      s->nowNs - controller->sinceNs >=
          (uint64_t)s->cfg->maxGreen * s->tickNs ||
      (s->cfg->ticks && s->nowNs >= s->cfg->ticks * s->tickNs) ||
      !flow->count || flow->occupied ||
      flow->items[flow->head].id != p->participantId ||
      (s->cfg->target && s->passed + s->active >= s->cfg->target))
    return false;
  for (unsigned i = 0; i < s->cfg->streamCount; ++i)
    if (s->flows[i].occupied &&
        (s->cfg->conflicts[p->stream] & (UINT32_C(1) << i)))
      return false;
  return true;
}

//------------------------------------------------------------------------------
static void EnterLocked(Simulation *s, ParticipantWorker *p) {
  Flow *flow = &s->flows[p->stream];
  uint64_t duration =
      (uint64_t)s->cfg->streams[p->stream].crossingTicks * s->tickNs;
  if (s->nowNs > UINT64_MAX - duration) {
    SimulationStopLocked(s, STOP_RUNTIME_ERROR, 0);
    return;
  }
  const char *name = s->cfg->streams[p->stream].name;
  flow->active = flow->items[flow->head];
  flow->head = (flow->head + 1) % s->cfg->streams[p->stream].capacity;
  --flow->count;
  flow->occupied = true;
  flow->enteredNs = s->nowNs;
  flow->leaveAtNs = s->nowNs + duration;
  ++s->active;
  SimulationEventLocked(s, "Разрешение: поток=%s id=%" PRIu64, name,
                        p->participantId);
  SimulationEventLocked(s, "Въезд: поток=%s id=%" PRIu64 " длина=%u", name,
                        p->participantId, flow->count);
}

//------------------------------------------------------------------------------
static void LeaveLocked(Simulation *s, ParticipantWorker *p) {
  Flow *flow = &s->flows[p->stream];
  uint64_t wait = (flow->enteredNs - flow->active.arrivedNs) / s->tickNs;
  flow->occupied = false;
  --s->active;
  ++s->passed;
  ++flow->passed;
  flow->waitSum += wait;
  if (wait > flow->waitMax)
    flow->waitMax = wait;
  SimulationEventLocked(s, "Выход: поток=%s id=%" PRIu64 " ожидание=%" PRIu64,
                        s->cfg->streams[p->stream].name, p->participantId,
                        wait);
  if (s->cfg->target && s->passed == s->cfg->target)
    SimulationStopLocked(s, STOP_TARGET, 0);
  SimulationWakeHeadsLocked(s);
}

//------------------------------------------------------------------------------
void *ParticipantRun(void *argument) {
  ParticipantWorker *p = argument;
  Simulation *s = p->simulation;
  SimulationLock(s);
  while (SimulationUpdateLocked(s) && !CanEnterLocked(s, p)) {
    SimulationUnlock(s);
    SimulationWait(s, p->worker.event, UINT64_MAX);
    SimulationLock(s);
  }
  if (s->stopReason == STOP_NONE) {
    EnterLocked(s, p);
    if (!SimulationCheckLocked(s))
      SimulationStopLocked(s, STOP_INVARIANT, 0);
    uint64_t deadline = s->flows[p->stream].leaveAtNs;
    while (SimulationUpdateLocked(s) && s->nowNs < deadline) {
      SimulationUnlock(s);
      SimulationWait(s, p->worker.event, deadline);
      SimulationLock(s);
    }
    if (s->stopReason == STOP_NONE)
      LeaveLocked(s, p);
  }
  if (!SimulationCheckLocked(s))
    SimulationStopLocked(s, STOP_INVARIANT, 0);
  p->worker.finished = true;
  SimulationNotifyLocked(s, &s->directionThreads[p->stream].worker);
  SimulationNotifyLocked(s, &s->controllerThread);
  SimulationUnlock(s);
  return NULL;
}

//------------------------------------------------------------------------------
// Направление собирает свои потоки; после его завершения это делает главный
// поток.
void ParticipantJoin(Simulation *s, ParticipantWorker *p) {
  SimulationLock(s);
  if (!p->worker.started) {
    SimulationUnlock(s);
    return;
  }
  pthread_t thread = p->worker.thread;
  SimulationUnlock(s);
  int error = pthread_join(thread, NULL);
  if (error) {
    errno = error;
    perror("pthread_join");
    abort();
  }
  SimulationLock(s);
  p->worker.started = p->worker.finished = false;
  p->participantId = 0;
  --s->participantCount;
  while (sem_trywait(p->worker.event) == 0 || errno == EINTR) {
  }
  if (errno != EAGAIN)
    SimulationStopLocked(s, STOP_RUNTIME_ERROR, 0);
  SimulationUnlock(s);
}

//------------------------------------------------------------------------------
static void ReapParticipants(DirectionWorker *direction) {
  Simulation *s = direction->simulation;
  SimulationLock(s);
  for (unsigned i = 0; i < direction->participantLimit; ++i) {
    ParticipantWorker *p = &s->participants[direction->firstParticipant + i];
    if (p->worker.started && p->worker.finished) {
      SimulationUnlock(s);
      ParticipantJoin(s, p);
      SimulationLock(s);
    }
  }
  SimulationUnlock(s);
}

//------------------------------------------------------------------------------
static ParticipantWorker *
FreeParticipantLocked(Simulation *s, const DirectionWorker *direction) {
  for (unsigned i = 0; i < direction->participantLimit; ++i) {
    ParticipantWorker *p = &s->participants[direction->firstParticipant + i];
    if (!p->worker.started)
      return p;
  }
  return NULL;
}

//------------------------------------------------------------------------------
static void ArriveLocked(Simulation *s, DirectionWorker *direction,
                         ParticipantWorker *worker) {
  unsigned i = direction->stream;
  const Stream *stream = &s->cfg->streams[i];
  Flow *flow = &s->flows[i];
  Participant p = {.id = ++s->nextId,
                   .arrivedNs = s->nowNs,
                   .emergency =
                       stream->kind == CAR &&
                       ParticipantRandom(s->cfg->seed, i, s->tick, true) <
                           s->cfg->emergencyRate};
  ++flow->arrived;
  SimulationEventLocked(s, "Прибытие: поток=%s id=%" PRIu64 " экстренный=%u",
                        stream->name, p.id, p.emergency);
  if (flow->count == stream->capacity) {
    ++flow->refused;
    SimulationEventLocked(s, "Отказ: поток=%s id=%" PRIu64 " длина=%u",
                          stream->name, p.id, flow->count);
    return;
  }
  worker->participantId = p.id;
  int error =
      pthread_create(&worker->worker.thread, NULL, ParticipantRun, worker);
  if (error) {
    worker->participantId = 0;
    ++flow->refused;
    SimulationEventLocked(s,
                          "Отказ: поток=%s id=%" PRIu64
                          " длина=%u причина=ошибка_потока код=%d",
                          stream->name, p.id, flow->count, error);
    SimulationStopLocked(s, STOP_RUNTIME_ERROR, 0);
    return;
  }
  worker->worker.started = true;
  ++s->participantCount;
  ++s->participantsCreated;
  flow->items[(flow->head + flow->count++) % stream->capacity] = p;
  if (flow->count > flow->peak)
    flow->peak = flow->count;
  SimulationEventLocked(s, "Очередь: поток=%s id=%" PRIu64 " длина=%u",
                        stream->name, p.id, flow->count);
  if (stream->kind == PEDESTRIAN)
    SimulationEventLocked(s, "Запрос пешехода: поток=%s id=%" PRIu64,
                          stream->name, p.id);
  if (p.emergency)
    SimulationEventLocked(
        s, "Запрос приоритета: поток=%s id=%" PRIu64 " включен=%u",
        stream->name, p.id, s->cfg->priority);
  if (flow->count == 1)
    SimulationNotifyLocked(s, &worker->worker);
  SimulationNotifyLocked(s, &s->controllerThread);
}

//------------------------------------------------------------------------------
void *DirectionRun(void *argument) {
  DirectionWorker *direction = argument;
  Simulation *s = direction->simulation;
  SimulationWait(s, direction->worker.event, UINT64_MAX);
  uint64_t previous = UINT64_MAX;
  for (;;) {
    ReapParticipants(direction);
    SimulationLock(s);
    if (!SimulationUpdateLocked(s)) {
      SimulationUnlock(s);
      break;
    }
    if (s->tick != previous && (!s->cfg->ticks || s->tick < s->cfg->ticks)) {
      const Stream *stream = &s->cfg->streams[direction->stream];
      if (ParticipantRandom(s->cfg->seed, direction->stream, s->tick, false) <
          stream->rate) {
        ParticipantWorker *p = FreeParticipantLocked(s, direction);
        if (!p && s->flows[direction->stream].count < stream->capacity) {
          SimulationUnlock(s);
          continue;
        }
        ArriveLocked(s, direction, p);
      }
      previous = s->tick;
    }
    if (!SimulationCheckLocked(s))
      SimulationStopLocked(s, STOP_INVARIANT, 0);
    uint64_t deadline = (s->tick + 1) * s->tickNs;
    bool running = s->stopReason == STOP_NONE;
    SimulationUnlock(s);
    if (!running || !SimulationWait(s, direction->worker.event, deadline))
      break;
  }
  SimulationLock(s);
  direction->worker.finished = true;
  SimulationNotifyLocked(s, &s->controllerThread);
  SimulationUnlock(s);
  return NULL;
}
