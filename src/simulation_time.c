//==============================================================================
// Монотонное время и ожидания вне мьютекса модели.
//==============================================================================
#include "simulation.h"

#include <errno.h>
#include <time.h>

//------------------------------------------------------------------------------
static bool ClockNow(uint64_t *value) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    return false;
  if (now.tv_sec < 0 || (uint64_t)now.tv_sec >
                            (UINT64_MAX - (uint64_t)now.tv_nsec) / 1000000000) {
    errno = EOVERFLOW;
    return false;
  }
  *value = (uint64_t)now.tv_sec * 1000000000 + (uint64_t)now.tv_nsec;
  return true;
}

//------------------------------------------------------------------------------
bool SimulationStartTimeLocked(Simulation *s) {
  if (!s->tickNs || !ClockNow(&s->startNs) ||
      s->cfg->ticks > (UINT64_MAX - s->startNs) / s->tickNs) {
    SimulationStopLocked(s, STOP_RUNTIME_ERROR, 0);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool SimulationUpdateLocked(Simulation *s) {
  if (s->stopReason != STOP_NONE)
    return false;
  uint64_t now;
  if (!ClockNow(&now) || now < s->startNs) {
    SimulationStopLocked(s, STOP_RUNTIME_ERROR, 0);
    return false;
  }
  s->nowNs = now - s->startNs;
  if (s->nowNs > UINT64_MAX - s->tickNs || s->nowNs > UINT64_MAX - 1000000) {
    SimulationStopLocked(s, STOP_RUNTIME_ERROR, 0);
    return false;
  }
  s->tick = s->nowNs / s->tickNs;
  if (s->out->failed)
    SimulationStopLocked(s, STOP_OUTPUT_ERROR, 0);
  else if (s->cfg->ticks && s->nowNs > s->cfg->ticks * s->tickNs)
    SimulationStopLocked(s, STOP_PERIOD, 0);
  return s->stopReason == STOP_NONE;
}

//------------------------------------------------------------------------------
// Короткий nanosleep делает ожидание срока прерываемым и в Linux, и в macOS.
bool SimulationWait(Simulation *s, sem_t *event, uint64_t deadlineNs) {
  for (;;) {
    if (deadlineNs != UINT64_MAX) {
      SimulationLock(s);
      bool running = SimulationUpdateLocked(s);
      uint64_t remaining = deadlineNs > s->nowNs ? deadlineNs - s->nowNs : 0;
      SimulationUnlock(s);
      if (!running)
        return false;
      if (!remaining)
        return true;
      if (sem_trywait(event) == 0)
        return true;
      if (errno == EINTR)
        continue;
      if (errno != EAGAIN)
        break;
      struct timespec delay = {
          .tv_nsec = (long)(remaining < 1000000 ? remaining : 1000000)};
      if (nanosleep(&delay, NULL) < 0 && errno != EINTR)
        break;
    } else {
      if (sem_wait(event) == 0)
        return true;
      if (errno != EINTR)
        break;
    }
  }
  SimulationLock(s);
  SimulationStopLocked(s, STOP_RUNTIME_ERROR, 0);
  SimulationUnlock(s);
  return false;
}
