//==============================================================================
// Инициализация состояния, остановка и проверка инвариантов.
//==============================================================================
#include "simulation.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

//------------------------------------------------------------------------------
// Имя удаляется сразу после открытия; семафор живёт до закрытия дескриптора.
static sem_t *EventOpen(void) {
  static atomic_uint sequence;
  for (;;) {
    unsigned number = atomic_fetch_add(&sequence, 1);
    char name[32];
    int length = snprintf(name, sizeof(name), "/cr-%lx-%x",
                          (unsigned long)getpid(), number);
    if (length < 0 || (size_t)length >= sizeof(name)) {
      errno = ENAMETOOLONG;
      return NULL;
    }
    sem_t *event = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
    if (event == SEM_FAILED) {
      if (errno == EEXIST)
        continue;
      return NULL;
    }
    if (sem_unlink(name) < 0) {
      int error = errno;
      sem_close(event);
      errno = error;
      return NULL;
    }
    return event;
  }
}

//------------------------------------------------------------------------------
static void WorkerClose(Worker *worker, int *error) {
  if (!worker->event)
    return;
  if (sem_close(worker->event) < 0 && !*error)
    *error = errno;
  worker->event = NULL;
}

//------------------------------------------------------------------------------
// Закрываются и семафоры частично подготовленного состояния.
static bool EventsClose(Simulation *s) {
  int error = 0;
  WorkerClose(&s->controllerThread, &error);
  for (unsigned i = 0; i < s->cfg->streamCount; ++i)
    WorkerClose(&s->directionThreads[i].worker, &error);
  for (unsigned i = 0; i < s->participantLimit; ++i)
    WorkerClose(&s->participants[i].worker, &error);
  if (error)
    errno = error;
  return !error;
}

//------------------------------------------------------------------------------
// Конфигурация должна быть проверена до создания состояния запуска.
bool SimulationInit(Simulation *s, const Config *cfg, Output *out) {
  *s = (Simulation){.cfg = cfg, .out = out};
  s->tickNs = (uint64_t)cfg->tickMs * 1000000;
  for (unsigned i = 0; i < cfg->streamCount; ++i) {
    DirectionWorker *direction = &s->directionThreads[i];
    direction->simulation = s;
    direction->stream = i;
    direction->firstParticipant = s->participantLimit;
    direction->participantLimit = cfg->streams[i].capacity + 1;
    for (unsigned j = 0; j < direction->participantLimit; ++j) {
      ParticipantWorker *participant = &s->participants[s->participantLimit++];
      participant->simulation = s;
      participant->stream = i;
    }
  }
  int error = pthread_mutex_init(&s->mutex, NULL);
  if (error) {
    errno = error;
    return false;
  }
  s->controllerThread.event = EventOpen();
  if (!s->controllerThread.event)
    goto fail;
  for (unsigned i = 0; i < cfg->streamCount; ++i) {
    s->directionThreads[i].worker.event = EventOpen();
    if (!s->directionThreads[i].worker.event)
      goto fail;
  }
  for (unsigned i = 0; i < s->participantLimit; ++i) {
    s->participants[i].worker.event = EventOpen();
    if (!s->participants[i].worker.event)
      goto fail;
  }
  return true;

fail:
  error = errno;
  EventsClose(s);
  pthread_mutex_destroy(&s->mutex);
  errno = error;
  return false;
}

//------------------------------------------------------------------------------
// Завершившийся, но ещё не собранный поток также удерживает своё состояние.
bool SimulationDestroy(Simulation *s) {
  if (s->controllerThread.started || s->participantCount) {
    errno = EBUSY;
    return false;
  }
  for (unsigned i = 0; i < s->cfg->streamCount; ++i)
    if (s->directionThreads[i].worker.started) {
      errno = EBUSY;
      return false;
    }
  for (unsigned i = 0; i < s->participantLimit; ++i)
    if (s->participants[i].worker.started) {
      errno = EBUSY;
      return false;
    }
  bool ok = EventsClose(s);
  int error = ok ? 0 : errno;
  int mutexError = pthread_mutex_destroy(&s->mutex);
  if (!error)
    error = mutexError;
  if (error)
    errno = error;
  return !error;
}

//------------------------------------------------------------------------------
void SimulationLock(Simulation *s) {
  int error = pthread_mutex_lock(&s->mutex);
  if (error) {
    errno = error;
    perror("pthread_mutex_lock");
    abort();
  }
}

//------------------------------------------------------------------------------
void SimulationUnlock(Simulation *s) {
  int error = pthread_mutex_unlock(&s->mutex);
  if (error) {
    errno = error;
    perror("pthread_mutex_unlock");
    abort();
  }
}

//------------------------------------------------------------------------------
const char *SimulationStopName(StopReason reason) {
  switch (reason) {
  case STOP_NONE:
    return "работает";
  case STOP_TARGET:
    return "цель";
  case STOP_PERIOD:
    return "период";
  case STOP_SIGNAL:
    return "прерывание";
  case STOP_EXHAUSTED:
    return "исчерпан";
  case STOP_UNREACHABLE:
    return "цель_недостижима";
  case STOP_OUTPUT_ERROR:
    return "ошибка_вывода";
  case STOP_INVARIANT:
    return "нарушен_инвариант";
  case STOP_RUNTIME_ERROR:
    return "ошибка_выполнения";
  }
  return "неизвестно";
}

//------------------------------------------------------------------------------
static void WorkerNotify(Worker *worker, int *error) {
  if (worker->started && sem_post(worker->event) < 0 && !*error)
    *error = errno;
}

//------------------------------------------------------------------------------
// Первая причина сохраняется; уведомляются все созданные потоки без ожиданий.
bool SimulationStopLocked(Simulation *s, StopReason reason, int signalNumber) {
  if (reason <= STOP_NONE || reason > STOP_RUNTIME_ERROR ||
      (reason == STOP_SIGNAL &&
       (signalNumber <= 0 || signalNumber > INT_MAX - 128))) {
    errno = EINVAL;
    return false;
  }
  if (s->stopReason != STOP_NONE)
    return true;
  s->stopReason = reason;
  s->signalNumber = reason == STOP_SIGNAL ? signalNumber : 0;
  switch (reason) {
  case STOP_SIGNAL:
    s->status = 128 + signalNumber;
    break;
  case STOP_UNREACHABLE:
    s->status = 3;
    break;
  case STOP_OUTPUT_ERROR:
  case STOP_INVARIANT:
  case STOP_RUNTIME_ERROR:
    s->status = 1;
    break;
  default:
    s->status = 0;
    break;
  }
  int error = 0;
  WorkerNotify(&s->controllerThread, &error);
  for (unsigned i = 0; i < s->cfg->streamCount; ++i)
    WorkerNotify(&s->directionThreads[i].worker, &error);
  for (unsigned i = 0; i < s->participantLimit; ++i)
    WorkerNotify(&s->participants[i].worker, &error);
  if (error) {
    s->status = 1;
    errno = error;
  }
  return !error;
}

//------------------------------------------------------------------------------
bool SimulationCheckLocked(const Simulation *s) {
  if (s->controllerThread.finished && !s->controllerThread.started)
    return false;
  uint32_t occupied = 0;
  unsigned active = 0, participants = 0;
  uint64_t passed = 0;
  for (unsigned i = 0; i < s->cfg->streamCount; ++i) {
    if (s->directionThreads[i].worker.finished &&
        !s->directionThreads[i].worker.started)
      return false;
    const Flow *f = &s->flows[i];
    unsigned capacity = s->cfg->streams[i].capacity;
    if (f->count > capacity || f->head >= capacity)
      return false;
    uint64_t previous = 0;
    for (unsigned j = 0; j < f->count; ++j) {
      const Participant *p = &f->items[(f->head + j) % capacity];
      if (!p->id || p->id <= previous || p->arrivedNs > s->nowNs ||
          (f->occupied && p->id == f->active.id))
        return false;
      previous = p->id;
    }
    if (f->occupied) {
      if (!f->active.id || f->active.arrivedNs > f->enteredNs ||
          f->enteredNs > s->nowNs || f->leaveAtNs < f->enteredNs ||
          !(s->controller.signals & (UINT32_C(1) << i)) ||
          (s->cfg->conflicts[i] & occupied))
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
  for (unsigned i = 0; i < s->participantLimit; ++i) {
    const ParticipantWorker *participant = &s->participants[i];
    if (participant->worker.finished && !participant->worker.started)
      return false;
    if (participant->worker.started) {
      if (!participant->participantId ||
          participant->stream >= s->cfg->streamCount)
        return false;
      ++participants;
    }
  }
  return active == s->active && passed == s->passed &&
         participants == s->participantCount &&
         s->participantsCreated >= participants &&
         (s->controller.state != CLEARANCE ||
          (!active && !s->controller.signals)) &&
         (!s->cfg->target || passed + active <= s->cfg->target);
}
