//==============================================================================
// Типы данных и интерфейсы модели регулируемого перекрёстка.
//==============================================================================
#ifndef CROSSROAD_H
#define CROSSROAD_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_STREAMS 16
#define MAX_PHASES 16
#define MAX_QUEUE 64
#define NAME_SIZE 32
#define TEXT_SIZE 16384

typedef enum Kind { CAR, PEDESTRIAN } Kind;
typedef enum Direction { NORTH, EAST, SOUTH, WEST } Direction;
typedef enum Maneuver { STRAIGHT, RIGHT, LEFT, WALK } Maneuver;
typedef enum Strategy { FIXED, ADAPTIVE } Strategy;

// Поток - отдельная полоса с одним манёвром или пешеходный переход.
typedef struct Stream {
  char name[NAME_SIZE];
  Kind kind;
  Direction direction;
  Maneuver maneuver;
  unsigned capacity, crossingTicks,
      rate; // rate: вероятность в тысячных долях на такт.
} Stream;

// Фаза разрешает одновременное движение совместимых потоков.
typedef struct Phase {
  char name[NAME_SIZE];
  uint32_t mask;
} Phase;

// Параметры, загружаемые из одного текстового файла.
typedef struct Config {
  Stream streams[MAX_STREAMS];
  Phase phases[MAX_PHASES];
  uint32_t conflicts[MAX_STREAMS]; // Симметричная матрица, диагональ равна 1.
  unsigned streamCount, phaseCount;
  unsigned minGreen, maxGreen, clearance, emergencyRate, delayMs, seed;
  uint64_t ticks, target;
  Strategy strategy;
  bool priority;
} Config;

// Участник принадлежит одной очереди или активному месту своего потока.
typedef struct Participant {
  uint64_t id, arrived;
  bool emergency;
} Participant;

// Кольцевая очередь FIFO, текущий проезд и статистика потока.
typedef struct Flow {
  Participant items[MAX_QUEUE];
  unsigned head, count, peak;
  bool occupied;
  Participant active;
  uint64_t leaveAt, arrived, refused, passed, waitSum, waitMax;
} Flow;

typedef enum ControllerState { GREEN, DRAIN, CLEARANCE } ControllerState;
// Контроллер хранит текущую фазу и начало её состояния.
typedef struct Controller {
  unsigned phase, nextFixed;
  ControllerState state;
  uint64_t since;
  uint32_t signals;
} Controller;

// Ошибки консоли и журнала учитываются независимо.
typedef struct Output {
  int logFd;
  bool failed, consoleFailed, logFailed;
} Output;

extern volatile sig_atomic_t stopSignal;
extern const char *kindNames[2];
extern const char *directionNames[4];
extern const char *maneuverNames[4];

// Чтение ограниченного текстового файла через файловый дескриптор.
bool ReadText(int fd, char *buffer, size_t capacity);
// Открытие и закрытие журнала.
bool OutputOpen(Output *out, const char *path);
void OutputClose(Output *out);
// Одно сообщение одновременно для консоли и журнала.
void OutputEvent(Output *out, uint64_t tick, const char *description);
// Загрузка параметров и проверка совместимости фаз.
bool ConfigLoad(Config *cfg, const char *path);
bool ConfigValidate(const Config *cfg);
// Последовательная имитация всех участников.
int SimulationRun(const Config *cfg, Output *out);

#endif // CROSSROAD_H
