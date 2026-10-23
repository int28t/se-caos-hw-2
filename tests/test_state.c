//==============================================================================
// Проверка остановки ожидающих потоков и времени жизни общего состояния.
//==============================================================================
#undef clock_gettime
#include "simulation.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      exit(EXIT_FAILURE);                                                      \
    }                                                                          \
  } while (0)

typedef struct WaitContext {
  Simulation *simulation;
  Worker *worker;
  bool observedStop;
  int error;
} WaitContext;

static uint64_t clockNs;

//------------------------------------------------------------------------------
int TestClockGettime(clockid_t clock, struct timespec *now) {
  CHECK(clock == CLOCK_MONOTONIC);
  *now = (struct timespec){.tv_sec = (time_t)(clockNs / 1000000000),
                           .tv_nsec = (long)(clockNs % 1000000000)};
  return 0;
}

//------------------------------------------------------------------------------
static void *WaitForStop(void *argument) {
  WaitContext *context = argument;
  int result;
  do {
    result = sem_wait(context->worker->event);
  } while (result < 0 && errno == EINTR);
  if (result < 0) {
    context->error = errno;
    return NULL;
  }
  Simulation *s = context->simulation;
  SimulationLock(s);
  context->observedStop =
      s->stopReason == STOP_SIGNAL && s->signalNumber == SIGTERM &&
      s->status == 128 + SIGTERM && SimulationCheckLocked(s);
  context->worker->finished = true;
  SimulationUnlock(s);
  return NULL;
}

//------------------------------------------------------------------------------
static Config TestConfig(void) {
  Config cfg = {.streamCount = 2,
                .phaseCount = 2,
                .minGreen = 2,
                .maxGreen = 4,
                .tickMs = 1,
                .clearance = 1,
                .ticks = 10};
  cfg.streams[0].capacity = 2;
  cfg.streams[1].capacity = 1;
  cfg.streams[0].crossingTicks = cfg.streams[1].crossingTicks = 1;
  cfg.conflicts[0] = cfg.conflicts[1] = 3;
  cfg.phases[0].mask = 1;
  cfg.phases[1].mask = 2;
  return cfg;
}

//------------------------------------------------------------------------------
// Повторная остановка не перезаписывает причину и не добавляет уведомления.
static void TestStopAndJoin(void) {
  Config cfg = TestConfig();
  Output out = {.logFd = -1};
  Simulation s;
  CHECK(SimulationInit(&s, &cfg, &out));
  CHECK(s.participantLimit == 5);
  WaitContext contexts[] = {
      {.simulation = &s, .worker = &s.controllerThread},
      {.simulation = &s, .worker = &s.directionThreads[1].worker},
      {.simulation = &s, .worker = &s.participants[0].worker}};
  SimulationLock(&s);
  s.participants[0].participantId = 1;
  s.participants[0].stream = 0;
  for (unsigned i = 0; i < 3; ++i) {
    CHECK(pthread_create(&contexts[i].worker->thread, NULL, WaitForStop,
                         &contexts[i]) == 0);
    contexts[i].worker->started = true;
  }
  s.participantsCreated = s.participantCount = 1;
  CHECK(SimulationCheckLocked(&s));
  SimulationUnlock(&s);
  CHECK(!SimulationDestroy(&s) && errno == EBUSY);

  SimulationLock(&s);
  CHECK(SimulationStopLocked(&s, STOP_SIGNAL, SIGTERM));
  CHECK(SimulationStopLocked(&s, STOP_PERIOD, 0));
  SimulationUnlock(&s);
  for (unsigned i = 0; i < 3; ++i) {
    SimulationLock(&s);
    pthread_t thread = contexts[i].worker->thread;
    SimulationUnlock(&s);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(!contexts[i].error && contexts[i].observedStop);
    CHECK(sem_trywait(contexts[i].worker->event) < 0 && errno == EAGAIN);
  }
  CHECK(!SimulationDestroy(&s) && errno == EBUSY);
  SimulationLock(&s);
  for (unsigned i = 0; i < 3; ++i) {
    CHECK(contexts[i].worker->finished);
    contexts[i].worker->started = contexts[i].worker->finished = false;
  }
  s.participants[0].participantId = 0;
  s.participantCount = 0;
  CHECK(s.participantsCreated == 1 && SimulationCheckLocked(&s));
  SimulationUnlock(&s);
  CHECK(SimulationDestroy(&s));
}

//------------------------------------------------------------------------------
static void TestInvariants(void) {
  Config cfg = TestConfig();
  Output out = {.logFd = -1};
  Simulation s;
  CHECK(SimulationInit(&s, &cfg, &out));
  SimulationLock(&s);
  CHECK(SimulationCheckLocked(&s));
  s.controller.signals = 3;
  CHECK(!SimulationCheckLocked(&s));
  s.controller.signals = 0;
  s.flows[0].count = 3;
  CHECK(!SimulationCheckLocked(&s));
  s.flows[0].count = 0;
  s.participantCount = 1;
  CHECK(!SimulationCheckLocked(&s));
  s.participantCount = 0;
  CHECK(SimulationCheckLocked(&s));
  s.participants[0].participantId = 1;
  CHECK(!SimulationCheckLocked(&s));
  s.participants[0].participantId = 0;
  s.tick = 3;
  s.nowNs = 3 * s.tickNs;
  s.controller.signals = 1;
  s.active = 1;
  s.flows[0].arrived = 1;
  s.flows[0].occupied = true;
  s.flows[0].active = (Participant){.id = 1};
  s.flows[0].enteredNs = s.tickNs;
  s.flows[0].leaveAtNs = 2 * s.tickNs;
  CHECK(SimulationCheckLocked(&s));
  s.flows[0].active.id = 0;
  CHECK(!SimulationCheckLocked(&s));
  s.flows[0].active.id = 1;
  s.controller.signals = 0;
  CHECK(!SimulationCheckLocked(&s));
  s.controller.signals = 1;
  CHECK(SimulationCheckLocked(&s));
  CHECK(!SimulationStopLocked(&s, STOP_NONE, 0) && errno == EINVAL);
  CHECK(s.stopReason == STOP_NONE);
  SimulationUnlock(&s);
  CHECK(SimulationDestroy(&s));
}

//------------------------------------------------------------------------------
// Несколько состояний одновременно не разделяют семафоры уведомлений.
static void TestIndependentRuns(void) {
  Config cfg = TestConfig();
  Output out = {.logFd = -1};
  Simulation first, second;
  CHECK(SimulationInit(&first, &cfg, &out));
  CHECK(SimulationInit(&second, &cfg, &out));
  CHECK(sem_post(first.controllerThread.event) == 0);
  CHECK(sem_trywait(second.controllerThread.event) < 0 && errno == EAGAIN);
  CHECK(sem_trywait(first.controllerThread.event) == 0);
  CHECK(SimulationDestroy(&second));
  CHECK(SimulationDestroy(&first));
}

//------------------------------------------------------------------------------
static void TestPeriodBoundary(void) {
  Config cfg = TestConfig();
  Output out = {.logFd = -1};
  Simulation s;
  CHECK(SimulationInit(&s, &cfg, &out));
  SimulationLock(&s);
  clockNs = UINT64_C(1000000000);
  CHECK(SimulationStartTimeLocked(&s));
  uint64_t boundary = cfg.ticks * s.tickNs;
  s.flows[0].arrived = s.flows[0].count = 1;
  s.flows[0].items[0] = (Participant){.id = 1};
  s.flows[1].arrived = 1;
  s.flows[1].occupied = true;
  s.flows[1].active = (Participant){.id = 2};
  s.flows[1].leaveAtNs = s.tickNs;
  s.active = 1;
  s.controller.signals = 2;
  clockNs = s.startNs + boundary - 1;
  CHECK(SimulationUpdateLocked(&s) && s.tick == cfg.ticks - 1);
  ++clockNs;
  CHECK(SimulationUpdateLocked(&s) && s.nowNs == boundary);
  ++clockNs;
  CHECK(!SimulationUpdateLocked(&s) && s.stopReason == STOP_PERIOD);
  CHECK(s.nowNs == boundary + 1);
  clockNs += 100 * s.tickNs;
  CHECK(!SimulationUpdateLocked(&s) && s.nowNs == boundary + 1);
  CHECK(SimulationStopLocked(&s, STOP_SIGNAL, SIGINT));
  CHECK(s.stopReason == STOP_PERIOD && !s.passed && s.active == 1);
  CHECK(s.flows[0].count == 1 && s.flows[1].occupied);
  CHECK(SimulationCheckLocked(&s));
  SimulationUnlock(&s);
  CHECK(SimulationDestroy(&s));
}

//------------------------------------------------------------------------------
int main(void) {
  TestInvariants();
  TestIndependentRuns();
  TestPeriodBoundary();
  for (unsigned i = 0; i < 20; ++i)
    TestStopAndJoin();
  return 0;
}
