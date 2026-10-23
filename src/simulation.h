//==============================================================================
// Общее состояние запуска и правила синхронизации.
//==============================================================================
#ifndef SIMULATION_H
#define SIMULATION_H

#include "crossroad.h"

#include <pthread.h>
#include <semaphore.h>

#define MAX_PARTICIPANTS (MAX_STREAMS * (MAX_QUEUE + 1))

typedef enum StopReason {
  STOP_NONE,
  STOP_TARGET,
  STOP_PERIOD,
  STOP_SIGNAL,
  STOP_EXHAUSTED,
  STOP_UNREACHABLE,
  STOP_OUTPUT_ERROR,
  STOP_INVARIANT,
  STOP_RUNTIME_ERROR
} StopReason;

// event не меняется после инициализации; остальные поля защищены mutex модели.
// started означает, что созданный поток ещё не собран через pthread_join.
typedef struct Worker {
  pthread_t thread;
  sem_t *event;
  bool started, finished;
} Worker;

typedef struct Simulation Simulation;

typedef struct ParticipantWorker {
  Worker worker;
  Simulation *simulation;
  uint64_t participantId;
  unsigned stream;
} ParticipantWorker;

typedef struct DirectionWorker {
  Worker worker;
  Simulation *simulation;
  unsigned stream, firstParticipant, participantLimit;
} DirectionWorker;

struct Simulation {
  // Задаются до запуска потоков и не меняются до их завершения.
  const Config *cfg;
  Output *out;
  unsigned participantLimit;

  // Все изменяемые поля ниже, включая *out, доступны только под mutex.
  pthread_mutex_t mutex;
  Flow flows[MAX_STREAMS];
  Controller controller;
  uint64_t tick, nextId, passed;
  uint64_t startNs, nowNs, tickNs, eventNumber;
  unsigned active;
  StopReason stopReason;
  int signalNumber, status;

  Worker controllerThread;
  DirectionWorker directionThreads[MAX_STREAMS];
  ParticipantWorker participants[MAX_PARTICIPANTS];
  uint64_t participantsCreated;
  unsigned participantCount;
};

// Инициализация до публикации состояния; уничтожение только после всех join.
bool SimulationInit(Simulation *s, const Config *cfg, Output *out);
bool SimulationDestroy(Simulation *s);
// Ошибка блокировки означает нарушение правил работы с состоянием.
void SimulationLock(Simulation *s);
void SimulationUnlock(Simulation *s);
// Вызываются с захваченным mutex; ожиданий и захвата mutex внутри нет.
bool SimulationCheckLocked(const Simulation *s);
bool SimulationStopLocked(Simulation *s, StopReason reason, int signalNumber);
const char *SimulationStopName(StopReason reason);

bool SimulationStartTimeLocked(Simulation *s);
bool SimulationUpdateLocked(Simulation *s);
// deadlineNs отсчитывается от начала модели; UINT64_MAX означает ожидание
// события.
bool SimulationWait(Simulation *s, sem_t *event, uint64_t deadlineNs);
void SimulationEventLocked(Simulation *s, const char *format, ...);
void SimulationNotifyLocked(Simulation *s, Worker *worker);
void SimulationWakeHeadsLocked(Simulation *s);

unsigned ParticipantRandom(unsigned seed, unsigned stream, uint64_t tick,
                           bool emergency);
void *DirectionRun(void *argument);
void *ParticipantRun(void *argument);
void ParticipantJoin(Simulation *s, ParticipantWorker *participant);

#endif // SIMULATION_H
