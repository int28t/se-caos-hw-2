//==============================================================================
// Проверка допуска, самостоятельных проездов и взаимодействия с контроллером.
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
static void StartParticipantLocked(Simulation *s, unsigned slot,
                                   bool emergency) {
  ParticipantWorker *p = &s->participants[slot];
  Flow *flow = &s->flows[p->stream];
  p->participantId = ++s->nextId;
  flow->items[(flow->head + flow->count++) %
              s->cfg->streams[p->stream].capacity] = (Participant){
      .id = p->participantId, .arrivedNs = s->nowNs, .emergency = emergency};
  ++flow->arrived;
  flow->peak = flow->count;
  CHECK(pthread_create(&p->worker.thread, NULL, ParticipantRun, p) == 0);
  p->worker.started = true;
  ++s->participantCount;
  ++s->participantsCreated;
  CHECK(SimulationCheckLocked(s));
}

//------------------------------------------------------------------------------
static void WaitForEntry(Simulation *s, unsigned stream, uint64_t id) {
  for (unsigned i = 0; i < 1000; ++i) {
    SimulationLock(s);
    bool entered =
        s->flows[stream].occupied && s->flows[stream].active.id == id;
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
  StartParticipantLocked(&s, 0, false);
  StartParticipantLocked(&s, 1, false);
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
  WaitForEntry(&s, 0, 1);
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
  StartParticipantLocked(&s, 0, false);
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
static void TestControllerPriorityAndDrain(bool interrupt) {
  Config cfg = TestConfig();
  cfg.streamCount = cfg.phaseCount = 3;
  cfg.tickMs = 10;
  cfg.minGreen = 2;
  cfg.maxGreen = cfg.streams[0].crossingTicks = 20;
  cfg.clearance = 2;
  cfg.priority = true;
  cfg.target = 3;
  cfg.streams[1] = (Stream){.name = "B", .capacity = 2, .crossingTicks = 1};
  cfg.streams[2] = (Stream){.name = "P",
                            .kind = PEDESTRIAN,
                            .maneuver = WALK,
                            .capacity = 1,
                            .crossingTicks = 1};
  cfg.phases[0] = (Phase){.name = "a", .mask = 1};
  cfg.phases[1] = (Phase){.name = "p", .mask = 4};
  cfg.phases[2] = (Phase){.name = "b", .mask = 2};
  for (unsigned i = 0; i < 3; ++i)
    cfg.conflicts[i] = 7;
  Output out;
  CHECK(OutputOpen(&out, "/dev/null"));
  Simulation s;
  CHECK(SimulationInit(&s, &cfg, &out));
  SimulationLock(&s);
  CHECK(pthread_create(&s.controllerThread.thread, NULL, ControllerRun, &s) ==
        0);
  s.controllerThread.started = true;
  CHECK(SimulationStartTimeLocked(&s));
  SimulationNotifyLocked(&s, &s.controllerThread);
  StartParticipantLocked(&s, 0, false);
  SimulationUnlock(&s);
  WaitForEntry(&s, 0, 1);
  SimulationLock(&s);
  CHECK(SimulationUpdateLocked(&s));
  uint64_t leaveAt = s.flows[0].leaveAtNs;
  unsigned firstB = s.directionThreads[1].firstParticipant;
  StartParticipantLocked(&s, firstB, false);
  StartParticipantLocked(&s, firstB + 1, true);
  SimulationNotifyLocked(&s, &s.controllerThread);
  SimulationUnlock(&s);

  bool drained = false;
  for (unsigned i = 0; i < 1000 && !drained; ++i) {
    SimulationLock(&s);
    drained = s.controller.state == DRAIN;
    if (drained) {
      CHECK(s.controller.phase == 0 && s.controller.signals == 1);
      CHECK(s.active == 1 && s.flows[0].occupied && s.flows[1].count == 2);
      CHECK(s.nowNs - s.controller.sinceNs >=
            (uint64_t)cfg.minGreen * s.tickNs);
      if (interrupt)
        CHECK(SimulationStopLocked(&s, STOP_SIGNAL, SIGTERM));
    }
    SimulationUnlock(&s);
    if (!drained)
      SleepMs(1);
  }
  CHECK(drained);
  ParticipantJoin(&s, &s.participants[0]);
  ParticipantJoin(&s, &s.participants[firstB]);
  ParticipantJoin(&s, &s.participants[firstB + 1]);
  SimulationLock(&s);
  pthread_t controller = s.controllerThread.thread;
  SimulationUnlock(&s);
  CHECK(pthread_join(controller, NULL) == 0);
  SimulationLock(&s);
  CHECK(s.controllerThread.finished);
  if (interrupt) {
    CHECK(s.stopReason == STOP_SIGNAL && s.passed == 0 && s.active == 1);
    CHECK(s.controller.state == DRAIN && s.flows[1].count == 2);
  } else {
    CHECK(s.stopReason == STOP_TARGET && s.passed == 3 && !s.active);
    CHECK(s.controller.phase == 2 && s.flows[1].active.id == 3);
    CHECK(s.controller.sinceNs >= leaveAt + (uint64_t)cfg.clearance * s.tickNs);
  }
  s.controllerThread.started = s.controllerThread.finished = false;
  CHECK(SimulationCheckLocked(&s));
  SimulationUnlock(&s);
  CHECK(SimulationDestroy(&s));
  OutputClose(&out);
}

//------------------------------------------------------------------------------
int main(void) {
  TestRandomSequence();
  TestStaleNotificationAndFIFO();
  TestExpiredGreenAndStop();
  TestControllerPriorityAndDrain(false);
  TestControllerPriorityAndDrain(true);
  return 0;
}
