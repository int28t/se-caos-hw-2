//==============================================================================
// Допуск после устаревшего уведомления и самостоятельное поведение участников.
//==============================================================================
#include "simulation.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

volatile sig_atomic_t stopSignal = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      exit(EXIT_FAILURE);                                                      \
    }                                                                          \
  } while (0)

//------------------------------------------------------------------------------
static void SleepMs(long milliseconds) {
  struct timespec delay = {.tv_sec = milliseconds / 1000,
                           .tv_nsec = (milliseconds % 1000) * 1000000};
  CHECK(nanosleep(&delay, NULL) == 0);
}

//------------------------------------------------------------------------------
static Config TestConfig(void) {
  Config cfg = {.streamCount = 1,
                .phaseCount = 1,
                .minGreen = 1,
                .maxGreen = 100,
                .clearance = 1,
                .tickMs = 5};
  cfg.streams[0] = (Stream){.name = "A", .capacity = 2, .crossingTicks = 4};
  cfg.conflicts[0] = cfg.phases[0].mask = 1;
  return cfg;
}

//------------------------------------------------------------------------------
static void StartParticipantLocked(Simulation *s, unsigned slot) {
  Flow *flow = &s->flows[0];
  ParticipantWorker *p = &s->participants[slot];
  p->participantId = ++s->nextId;
  flow->items[(flow->head + flow->count++) % s->cfg->streams[0].capacity] =
      (Participant){.id = p->participantId, .arrivedNs = s->nowNs};
  ++flow->arrived;
  flow->peak = flow->count;
  CHECK(pthread_create(&p->worker.thread, NULL, ParticipantRun, p) == 0);
  p->worker.started = true;
  ++s->participantCount;
  ++s->participantsCreated;
  CHECK(SimulationCheckLocked(s));
}

//------------------------------------------------------------------------------
static void WaitForEntry(Simulation *s, uint64_t id) {
  for (unsigned i = 0; i < 1000; ++i) {
    SimulationLock(s);
    bool entered = s->flows[0].occupied && s->flows[0].active.id == id;
    SimulationUnlock(s);
    if (entered)
      return;
    SleepMs(1);
  }
  CHECK(false);
}

//------------------------------------------------------------------------------
static void TestStaleNotificationAndFIFO(void) {
  Config cfg = TestConfig();
  Output out;
  CHECK(OutputOpen(&out, "/dev/null"));
  Simulation s;
  CHECK(SimulationInit(&s, &cfg, &out));
  SimulationLock(&s);
  CHECK(SimulationStartTimeLocked(&s));
  s.controller.signals = 1;
  StartParticipantLocked(&s, 0);
  StartParticipantLocked(&s, 1);
  SimulationNotifyLocked(&s, &s.participants[0].worker);
  SimulationNotifyLocked(&s, &s.participants[1].worker);
  s.controller.state = DRAIN;
  SimulationUnlock(&s);

  SleepMs(10);
  SimulationLock(&s);
  CHECK(s.flows[0].count == 2 && !s.active && !s.passed);
  CHECK(SimulationUpdateLocked(&s));
  s.controller.state = GREEN;
  s.controller.sinceNs = s.nowNs;
  SimulationWakeHeadsLocked(&s);
  SimulationUnlock(&s);
  WaitForEntry(&s, 1);
  SimulationLock(&s);
  uint64_t firstDeadline = s.flows[0].leaveAtNs;
  SimulationUnlock(&s);
  ParticipantJoin(&s, &s.participants[0]);
  ParticipantJoin(&s, &s.participants[1]);
  SimulationLock(&s);
  CHECK(s.passed == 2 && !s.active && !s.flows[0].count);
  CHECK(s.flows[0].active.id == 2 && s.flows[0].enteredNs >= firstDeadline);
  CHECK(SimulationCheckLocked(&s));
  SimulationUnlock(&s);
  CHECK(SimulationDestroy(&s));
  OutputClose(&out);
}

//------------------------------------------------------------------------------
static void TestExpiredGreenAndStop(void) {
  Config cfg = TestConfig();
  cfg.maxGreen = cfg.streams[0].crossingTicks = 1;
  Output out;
  CHECK(OutputOpen(&out, "/dev/null"));
  Simulation s;
  CHECK(SimulationInit(&s, &cfg, &out));
  SimulationLock(&s);
  CHECK(SimulationStartTimeLocked(&s));
  s.controller.signals = 1;
  SimulationNotifyLocked(&s, &s.participants[0].worker);
  SimulationUnlock(&s);
  SleepMs(15);
  SimulationLock(&s);
  CHECK(SimulationUpdateLocked(&s));
  StartParticipantLocked(&s, 0);
  SimulationUnlock(&s);
  SleepMs(10);
  SimulationLock(&s);
  CHECK(!s.active && !s.passed && s.flows[0].count == 1);
  CHECK(SimulationStopLocked(&s, STOP_SIGNAL, SIGTERM));
  SimulationUnlock(&s);
  ParticipantJoin(&s, &s.participants[0]);
  SimulationLock(&s);
  CHECK(!s.passed && s.flows[0].count == 1 && SimulationCheckLocked(&s));
  SimulationUnlock(&s);
  CHECK(SimulationDestroy(&s));
  OutputClose(&out);
}

//------------------------------------------------------------------------------
static void TestRandomSequence(void) {
  bool changed = false;
  for (uint64_t tick = 0; tick < 100; ++tick) {
    unsigned value = ParticipantRandom(39, 2, tick, false);
    CHECK(value < 1000 && value == ParticipantRandom(39, 2, tick, false));
    changed |= value != ParticipantRandom(40, 2, tick, false);
  }
  CHECK(changed);
}

//------------------------------------------------------------------------------
int main(void) {
  TestRandomSequence();
  TestStaleNotificationAndFIFO();
  TestExpiredGreenAndStop();
  return 0;
}
