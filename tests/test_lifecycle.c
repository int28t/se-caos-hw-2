//==============================================================================
// Лимит потоков, повторное использование контекстов и ошибки создания ресурсов.
//==============================================================================
#undef pthread_create
#undef pthread_join
#undef sem_open
#undef sem_close
#include "simulation.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

volatile sig_atomic_t stopSignal = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      exit(EXIT_FAILURE);                                                      \
    }                                                                          \
  } while (0)

typedef struct ThreadRecord {
  pthread_t thread;
  void *argument;
  ParticipantWorker *participant;
  sem_t *event;
  uint64_t id;
  bool used;
} ThreadRecord;

static pthread_mutex_t resourceMutex = PTHREAD_MUTEX_INITIALIZER;
static struct Resources {
  ThreadRecord threads[MAX_PARTICIPANTS + MAX_STREAMS + 1];
  Simulation *simulation;
  unsigned createCalls, joinCalls, openCalls, closeCalls;
  unsigned failCreate, failOpen;
  unsigned participants, peakParticipants, created;
  unsigned perStream[MAX_STREAMS];
} resources;

//------------------------------------------------------------------------------
int TestThreadCreate(pthread_t *thread, const pthread_attr_t *attributes,
                     void *(*run)(void *), void *argument) {
  CHECK(pthread_mutex_lock(&resourceMutex) == 0);
  Simulation *s;
  ParticipantWorker *p = NULL;
  if (run == ParticipantRun) {
    p = argument;
    s = p->simulation;
    CHECK(!p->worker.started && !p->worker.finished && p->participantId);
    CHECK(sem_trywait(p->worker.event) < 0 && errno == EAGAIN);
  } else if (run == DirectionRun)
    s = ((DirectionWorker *)argument)->simulation;
  else {
    CHECK(run == ControllerRun);
    s = argument;
  }
  resources.simulation = s;
  if (++resources.createCalls == resources.failCreate) {
    CHECK(pthread_mutex_unlock(&resourceMutex) == 0);
    return EAGAIN;
  }
  ThreadRecord *record = NULL;
  for (unsigned i = 0; i < MAX_PARTICIPANTS + MAX_STREAMS + 1; ++i) {
    ThreadRecord *candidate = &resources.threads[i];
    CHECK(!candidate->used || candidate->argument != argument);
    if (!candidate->used && !record)
      record = candidate;
  }
  CHECK(record);
  int error = pthread_create(thread, attributes, run, argument);
  CHECK(!error);
  *record = (ThreadRecord){.thread = *thread,
                           .argument = argument,
                           .participant = p,
                           .event = p ? p->worker.event : NULL,
                           .id = p ? p->participantId : 0,
                           .used = true};
  if (p) {
    ++resources.created;
    ++resources.participants;
    CHECK(resources.participants <= s->participantLimit);
    CHECK(++resources.perStream[p->stream] <=
          s->cfg->streams[p->stream].capacity + 1);
    if (resources.participants > resources.peakParticipants)
      resources.peakParticipants = resources.participants;
  }
  CHECK(pthread_mutex_unlock(&resourceMutex) == 0);
  return 0;
}

//------------------------------------------------------------------------------
int TestThreadJoin(pthread_t thread, void **result) {
  CHECK(pthread_mutex_lock(&resourceMutex) == 0);
  ThreadRecord *record = NULL;
  for (unsigned i = 0; i < MAX_PARTICIPANTS + MAX_STREAMS + 1; ++i)
    if (resources.threads[i].used &&
        pthread_equal(resources.threads[i].thread, thread)) {
      record = &resources.threads[i];
      break;
    }
  CHECK(record);
  CHECK(pthread_mutex_unlock(&resourceMutex) == 0);
  int error = pthread_join(thread, result);
  CHECK(!error);
  CHECK(pthread_mutex_lock(&resourceMutex) == 0);
  ParticipantWorker *p = record->participant;
  if (p) {
    CHECK(p->worker.started && p->worker.finished);
    CHECK(p->participantId == record->id && p->worker.event == record->event);
    --resources.participants;
    --resources.perStream[p->stream];
  }
  record->used = false;
  ++resources.joinCalls;
  CHECK(pthread_mutex_unlock(&resourceMutex) == 0);
  return 0;
}

//------------------------------------------------------------------------------
sem_t *TestSemOpen(const char *name, int flags, ...) {
  if (++resources.openCalls == resources.failOpen) {
    errno = EMFILE;
    return SEM_FAILED;
  }
  CHECK(flags & O_CREAT);
  va_list args;
  va_start(args, flags);
  mode_t mode = (mode_t)va_arg(args, int);
  unsigned value = va_arg(args, unsigned);
  va_end(args);
  sem_t *event = sem_open(name, flags, mode, value);
  CHECK(event != SEM_FAILED);
  return event;
}

//------------------------------------------------------------------------------
int TestSemClose(sem_t *event) {
  CHECK(pthread_mutex_lock(&resourceMutex) == 0);
  CHECK(!resources.participants &&
        resources.createCalls - (resources.failCreate != 0) ==
            resources.joinCalls);
  Simulation *s = resources.simulation;
  if (s) {
    CHECK(SimulationCheckLocked(s));
    CHECK(s->stopReason ==
          (resources.failCreate ? STOP_RUNTIME_ERROR : STOP_TARGET));
    CHECK(s->participantsCreated == resources.created);
    CHECK(!s->participantCount && !s->controllerThread.started);
    for (unsigned i = 0; i < s->cfg->streamCount; ++i)
      CHECK(!s->directionThreads[i].worker.started);
    for (unsigned i = 0; i < s->participantLimit; ++i)
      CHECK(!s->participants[i].worker.started &&
            !s->participants[i].participantId);
  }
  ++resources.closeCalls;
  CHECK(pthread_mutex_unlock(&resourceMutex) == 0);
  return sem_close(event);
}

//------------------------------------------------------------------------------
static Config TestConfig(void) {
  Config cfg = {.streamCount = 2,
                .phaseCount = 1,
                .minGreen = 1,
                .maxGreen = 20,
                .clearance = 1,
                .tickMs = 1,
                .target = 1000};
  cfg.streams[0] =
      (Stream){.name = "A", .capacity = 1, .crossingTicks = 1, .rate = 1000};
  cfg.streams[1] =
      (Stream){.name = "B", .capacity = 2, .crossingTicks = 1, .rate = 1000};
  cfg.conflicts[0] = 1;
  cfg.conflicts[1] = 2;
  cfg.phases[0] = (Phase){.name = "ab", .mask = 3};
  return cfg;
}

//------------------------------------------------------------------------------
static void Run(unsigned failCreate, unsigned failOpen) {
  resources =
      (struct Resources){.failCreate = failCreate, .failOpen = failOpen};
  Config cfg = TestConfig();
  Output out;
  CHECK(OutputOpen(&out, "/dev/null"));
  CHECK(SimulationRun(&cfg, &out) == (failCreate || failOpen ? 1 : 0));
  OutputClose(&out);
  CHECK(resources.openCalls - (failOpen != 0) == resources.closeCalls);
  CHECK(resources.createCalls - (failCreate != 0) == resources.joinCalls);
  if (failOpen)
    CHECK(!resources.createCalls && resources.openCalls == failOpen);
  else if (failCreate)
    CHECK(resources.createCalls == failCreate);
  else {
    CHECK(resources.created >= cfg.target);
    CHECK(resources.peakParticipants > 0 && resources.peakParticipants <= 5);
  }
  for (unsigned i = 0; i < MAX_PARTICIPANTS + MAX_STREAMS + 1; ++i)
    CHECK(!resources.threads[i].used);
}

//------------------------------------------------------------------------------
int main(void) {
  CHECK(freopen("/dev/null", "w", stdout));
  Run(0, 0);
  Run(0, 0);
  for (unsigned i = 1; i <= 6; ++i)
    Run(i, 0);
  for (unsigned i = 1; i <= 8; ++i)
    Run(0, i);
  return 0;
}
