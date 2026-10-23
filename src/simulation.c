//==============================================================================
// Запуск потоков, наблюдение и итоговая статистика.
//==============================================================================
#include "simulation.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

//------------------------------------------------------------------------------
// Формирование сообщения о событии.
void SimulationEventLocked(Simulation *s, const char *format, ...) {
  char text[1950];
  va_list args;
  va_start(args, format);
  int n = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (n < 0 || (size_t)n >= sizeof(text)) {
    s->out->failed = true;
    return;
  }
  size_t used = (size_t)n;
  n = snprintf(text + used, sizeof(text) - used,
               " событие=%" PRIu64 " время_нс=%" PRIu64, ++s->eventNumber,
               s->nowNs);
  if (n < 0 || (size_t)n >= sizeof(text) - used) {
    s->out->failed = true;
    return;
  }
  OutputEvent(s->out, s->tick, text);
}

//------------------------------------------------------------------------------
// Параметры и определения потоков позволяют проверить журнал независимо от
// модели.
static void ParametersOutLocked(Simulation *s) {
  const Config *c = s->cfg;
  SimulationEventLocked(
      s,
      "Запуск: стратегия=%s минимум=%u максимум=%u интервал=%u seed=%u "
      "цель=%" PRIu64 " период=%" PRIu64
      " приоритет=%u экстренные=%u такт_мс=%u задержка_мс=%u",
      c->strategy == FIXED ? "fixed" : "adaptive", c->minGreen, c->maxGreen,
      c->clearance, c->seed, c->target, c->ticks, c->priority, c->emergencyRate,
      c->tickMs, c->delayMs);
  for (unsigned i = 0; i < c->streamCount; ++i) {
    const Stream *f = &c->streams[i];
    SimulationEventLocked(
        s,
        "Поток: имя=%s тип=%s направление=%s маневр=%s емкость=%u "
        "пересечение=%u"
        " интенсивность=%u конфликты=%" PRIu32,
        f->name, kindNames[f->kind], directionNames[f->direction],
        maneuverNames[f->maneuver], f->capacity, f->crossingTicks, f->rate,
        c->conflicts[i]);
  }
  for (unsigned p = 0; p < c->phaseCount; ++p)
    SimulationEventLocked(s, "Фаза: имя=%s маска=%" PRIu32, c->phases[p].name,
                          c->phases[p].mask);
}

//------------------------------------------------------------------------------
// Итог учитывает прошедших, отказы, очереди и незавершённые пересечения.
static void StatisticsOutLocked(Simulation *s, const char *reason) {
  uint64_t arrived = 0, refused = 0, waitSum = 0, waitMax = 0;
  SimulationEventLocked(s, "Завершение: причина=%s сигнал=%d", reason,
                        s->signalNumber);
  for (unsigned i = 0; i < s->cfg->streamCount; ++i) {
    Flow *f = &s->flows[i];
    arrived += f->arrived;
    refused += f->refused;
    waitSum += f->waitSum;
    if (f->waitMax > waitMax)
      waitMax = f->waitMax;
    SimulationEventLocked(
        s,
        "Итог потока: имя=%s прибыло=%" PRIu64 " прошло=%" PRIu64
        " отказов=%" PRIu64 " ожидают=%u в_зоне=%u пик=%u ожидание=%" PRIu64
        " максимум=%" PRIu64,
        s->cfg->streams[i].name, f->arrived, f->passed, f->refused, f->count,
        f->occupied, f->peak, f->waitSum, f->waitMax);
  }
  SimulationEventLocked(
      s,
      "ИТОГО: прибыло=%" PRIu64 " прошло=%" PRIu64 " отказов=%" PRIu64
      " ожидают=%u в_зоне=%u ожидание=%" PRIu64
      " среднее=%.2f максимум=%" PRIu64 " потоков=%" PRIu64,
      arrived, s->passed, refused, SimulationQueueCountLocked(s), s->active,
      waitSum, s->passed ? (double)waitSum / (double)s->passed : 0.0, waitMax,
      s->participantsCreated);
}

//------------------------------------------------------------------------------
static void JoinWorker(Simulation *s, Worker *worker) {
  SimulationLock(s);
  if (!worker->started) {
    SimulationUnlock(s);
    return;
  }
  pthread_t thread = worker->thread;
  SimulationUnlock(s);
  int error = pthread_join(thread, NULL);
  if (error) {
    errno = error;
    perror("pthread_join");
    abort();
  }
  SimulationLock(s);
  worker->started = worker->finished = false;
  SimulationUnlock(s);
}

//------------------------------------------------------------------------------
int SimulationRun(const Config *cfg, Output *out) {
  Simulation s;
  if (!SimulationInit(&s, cfg, out)) {
    perror("Не удалось подготовить состояние модели");
    return 1;
  }
  sigset_t signals, previousMask;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  int error = pthread_sigmask(SIG_BLOCK, &signals, &previousMask);
  bool maskSaved = !error;
  SimulationLock(&s);
  ParametersOutLocked(&s);
  if (error)
    SimulationStopLocked(&s, STOP_RUNTIME_ERROR, 0);
  else if (out->failed)
    SimulationStopLocked(&s, STOP_OUTPUT_ERROR, 0);
  for (unsigned i = 0; i < cfg->streamCount && s.stopReason == STOP_NONE; ++i) {
    DirectionWorker *direction = &s.directionThreads[i];
    error = pthread_create(&direction->worker.thread, NULL, DirectionRun,
                           direction);
    if (error) {
      SimulationEventLocked(&s, "Ошибка: операция=pthread_create код=%d",
                            error);
      SimulationStopLocked(&s, STOP_RUNTIME_ERROR, 0);
    } else
      direction->worker.started = true;
  }
  if (s.stopReason == STOP_NONE) {
    error = pthread_create(&s.controllerThread.thread, NULL, ControllerRun, &s);
    if (error) {
      SimulationEventLocked(&s, "Ошибка: операция=pthread_create код=%d",
                            error);
      SimulationStopLocked(&s, STOP_RUNTIME_ERROR, 0);
    } else
      s.controllerThread.started = true;
  }
  if (s.stopReason == STOP_NONE && SimulationStartTimeLocked(&s))
    SimulationNotifyLocked(&s, &s.controllerThread);
  SimulationUnlock(&s);
  if (maskSaved) {
    error = pthread_sigmask(SIG_SETMASK, &previousMask, NULL);
    if (error) {
      SimulationLock(&s);
      SimulationStopLocked(&s, STOP_RUNTIME_ERROR, 0);
      SimulationUnlock(&s);
    }
  }
  bool arrivals = false, snapshot = false;
  uint64_t snapshotNs = 0, snapshotTick = 0;
  for (unsigned i = 0; i < cfg->streamCount; ++i)
    arrivals |= cfg->streams[i].rate != 0;
  for (;;) {
    SimulationLock(&s);
    if (SimulationUpdateLocked(&s) && stopSignal)
      SimulationStopLocked(&s, STOP_SIGNAL, (int)stopSignal);
    if (s.stopReason == STOP_NONE && !arrivals && !s.active &&
        !SimulationQueueCountLocked(&s))
      SimulationStopLocked(&s, cfg->target ? STOP_UNREACHABLE : STOP_EXHAUSTED,
                           0);
    if (s.stopReason != STOP_NONE) {
      SimulationUnlock(&s);
      break;
    }
    if (!SimulationCheckLocked(&s))
      SimulationStopLocked(&s, STOP_INVARIANT, 0);
    bool display =
        !snapshot ||
        (cfg->delayMs ? s.nowNs - snapshotNs >= (uint64_t)cfg->delayMs * 1000000
                      : s.tick != snapshotTick);
    if (display) {
      SimulationEventLocked(&s, "Состояние: ожидают=%u пересекают=%u",
                            SimulationQueueCountLocked(&s), s.active);
      snapshot = true;
      snapshotNs = s.nowNs;
      snapshotTick = s.tick;
    }
    uint64_t deadline = s.nowNs + 1000000;
    SimulationUnlock(&s);
    SimulationWait(&s, NULL, deadline);
  }
  JoinWorker(&s, &s.controllerThread);
  for (unsigned i = 0; i < cfg->streamCount; ++i)
    JoinWorker(&s, &s.directionThreads[i].worker);
  for (unsigned i = 0; i < s.participantLimit; ++i)
    ParticipantJoin(&s, &s.participants[i]);
  SimulationLock(&s);
  StatisticsOutLocked(&s, SimulationStopName(s.stopReason));
  int status = out->failed ? 1 : s.status;
  SimulationUnlock(&s);
  if (!SimulationDestroy(&s)) {
    perror("Не удалось освободить состояние модели");
    return 1;
  }
  return status;
}
