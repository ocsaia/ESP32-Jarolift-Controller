#include <Arduino.h>
#include <config.h>
#include <jarolift.h>
#include <message.h>
#include <shutterPos.h>

/* D E C L A R A T I O N S ****************************************************/

#define CH_COUNT 16

// Used when only the DOWN direction has been calibrated. A roller shutter is
// slower going up than coming down because the motor lifts the curtain instead
// of lowering it, so an UP travel derived from DOWN needs a margin. It is a
// fallback that keeps the feature usable after a single calibration run, not a
// substitute for measuring the second direction.
#define UP_FACTOR_NUM 12
#define UP_FACTOR_DEN 10

static const char *TAG = "POS"; // LOG TAG

struct s_shutterState {
  int8_t pos;         // current estimate, or SHUTTER_POS_UNKNOWN
  int8_t posAtStart;  // estimate when the motor started
  int8_t target;      // requested position, or -1 for "run to the end-stop"
  uint32_t startMs;   // millis() when the RF telegram went out
  uint32_t stopAtMs;  // absolute deadline for the STOP, 0 = no timed stop
  bool moving;        // the motor is believed to be running
  bool goingDown;     // direction of the current movement
  bool pendingMove;   // command queued, waiting for the telegram to go out
};

static s_shutterState shutter[CH_COUNT];

/**
 * *******************************************************************
 * @brief   full travel time for one direction
 * @param   channel, goingDown
 * @return  milliseconds, or 0 when the channel is not calibrated
 * *******************************************************************/
static uint32_t travelTime(uint8_t channel, bool goingDown) {
  if (goingDown) {
    return config.jaro.ch_travel_down[channel];
  }
  if (config.jaro.ch_travel_up[channel] > 0) {
    return config.jaro.ch_travel_up[channel];
  }
  return config.jaro.ch_travel_down[channel] * UP_FACTOR_NUM / UP_FACTOR_DEN;
}

/**
 * *******************************************************************
 * @brief   channel label for log output
 * @param   channel
 * @return  configured name, or "channel N" when it has none
 * *******************************************************************/
static const char *chName(uint8_t channel) {
  static char name[24];
  if (channel < CH_COUNT && config.jaro.ch_name[channel][0] != 0) {
    return config.jaro.ch_name[channel];
  }
  snprintf(name, sizeof(name), "channel %u", (unsigned)channel + 1);
  return name;
}

/**
 * *******************************************************************
 * @brief   estimate where the shutter is right now
 * @details pure - it reads the clock but changes nothing, so it can be called
 *          from anywhere without disturbing the state machine
 * @param   channel
 * @return  estimated position, or the stored one when not moving
 * *******************************************************************/
static int8_t estimatePos(uint8_t channel) {
  s_shutterState &st = shutter[channel];

  if (!st.moving || st.posAtStart == SHUTTER_POS_UNKNOWN) {
    return st.pos;
  }
  uint32_t travel = travelTime(channel, st.goingDown);
  if (travel == 0) {
    return st.pos;
  }

  uint32_t elapsed = millis() - st.startMs;
  // 64 bit on purpose: elapsed can reach the full travel time and travel times
  // are milliseconds, so the product overflows 32 bit well before that
  int32_t moved = (int32_t)((uint64_t)elapsed * 100u / travel);
  if (moved > 100) {
    moved = 100;
  }

  int32_t pos = st.goingDown ? (st.posAtStart - moved) : (st.posAtStart + moved);
  if (pos < 0) {
    pos = 0;
  } else if (pos > 100) {
    pos = 100;
  }
  return (int8_t)pos;
}

/**
 * *******************************************************************
 * @brief   settle a channel at a known position and stop tracking it
 * @param   channel, position
 * @return  none
 * *******************************************************************/
static void settleAt(uint8_t channel, int8_t position) {
  s_shutterState &st = shutter[channel];
  st.pos = position;
  st.posAtStart = position;
  st.target = -1;
  st.stopAtMs = 0;
  st.moving = false;
  st.pendingMove = false;
  if (position != SHUTTER_POS_UNKNOWN) {
    mqttSendPosition(channel, (uint8_t)position);
  }
}

/**
 * *******************************************************************
 * @brief   arm the stop deadline once the motor is known to be running
 * @details shared by the UP and DOWN notifications - the only difference is
 *          which way the percentage has to move to reach the target
 * @param   channel, goingDown
 * @return  none
 * *******************************************************************/
static void startMovement(uint8_t channel, bool goingDown) {
  s_shutterState &st = shutter[channel];

  // Not calibrated: there is nothing to interpolate, so report the end position
  // at once. That is exactly what the firmware did before position tracking
  // existed, and it keeps a channel usable without a calibration run.
  if (travelTime(channel, goingDown) == 0) {
    settleAt(channel, goingDown ? 0 : 100);
    return;
  }

  // a direction change while running: bank the estimate reached so far
  if (st.moving) {
    st.pos = estimatePos(channel);
  }

  st.posAtStart = st.pos;
  st.startMs = millis();
  st.goingDown = goingDown;
  st.moving = true;

  bool wantsTimedStop = st.pendingMove && st.target >= 0 && st.pos != SHUTTER_POS_UNKNOWN;
  st.pendingMove = false;

  if (!wantsTimedStop) {
    // no target, or no idea where we are: run to the end-stop, which is also
    // how an unknown position gets resolved
    st.stopAtMs = 0;
    st.target = -1;
    ESP_LOGI(TAG, "%s: %s to the end-stop", chName(channel), goingDown ? "DOWN" : "UP");
    return;
  }

  // The estimate can have drifted past the target between accepting the command
  // and the telegram going out - a queued command waits up to SEND_CYCLE, and a
  // service command can hold up the loop for seconds. Stop at once rather than
  // computing a negative span that would wrap into a very long run.
  int32_t span = goingDown ? (st.pos - st.target) : (st.target - st.pos);
  if (span <= 0) {
    st.stopAtMs = st.startMs;
    ESP_LOGI(TAG, "%s: already at or past %d%% - stopping immediately", chName(channel), st.target);
    return;
  }

  uint32_t runMs = (uint32_t)span * travelTime(channel, goingDown) / 100u;
  st.stopAtMs = st.startMs + runMs;
  ESP_LOGI(TAG, "%s: %s from %d%% to %d%%, stop in %lu ms", chName(channel), goingDown ? "DOWN" : "UP", st.pos, st.target,
           (unsigned long)runMs);
}

/**
 * *******************************************************************
 * @brief   Setup for position tracking
 * @param   none
 * @return  none
 * *******************************************************************/
void shutterPosSetup() {
  for (uint8_t i = 0; i < CH_COUNT; i++) {
    shutter[i].pos = SHUTTER_POS_UNKNOWN;
    shutter[i].posAtStart = SHUTTER_POS_UNKNOWN;
    shutter[i].target = -1;
    shutter[i].startMs = 0;
    shutter[i].stopAtMs = 0;
    shutter[i].moving = false;
    shutter[i].goingDown = false;
    shutter[i].pendingMove = false;
  }
  ESP_LOGI(TAG, "position tracking ready - %d channels, position unknown until a shutter reaches an end-stop", CH_COUNT);
}

/**
 * *******************************************************************
 * @brief   cyclic position tracking - fires the timed stops
 * @details called from jaroliftCyclic() on every loop() pass rather than on a
 *          timer: the deadline should be honoured as soon as it passes, and
 *          the loop is already delayed by whatever the radio is doing
 * @param   none
 * @return  none
 * *******************************************************************/
void shutterPosCyclic() {

  uint32_t now = millis();

  for (uint8_t ch = 0; ch < CH_COUNT; ch++) {
    s_shutterState &st = shutter[ch];

    if (!st.moving) {
      continue;
    }

    if (st.stopAtMs == 0) {
      // running to an end-stop: the position is only settled once a full travel
      // has elapsed, and then it is known exactly rather than estimated
      int8_t est = estimatePos(ch);
      if (st.goingDown && est <= 0) {
        settleAt(ch, 0);
        ESP_LOGI(TAG, "%s: closed", chName(ch));
      } else if (!st.goingDown && est >= 100) {
        settleAt(ch, 100);
        ESP_LOGI(TAG, "%s: open", chName(ch));
      }
      continue;
    }

    // Signed comparison so a millis() rollover during a movement does not look
    // like a deadline far in the future.
    if ((int32_t)(now - st.stopAtMs) >= 0) {
      int8_t landed = st.target;
      jaroStopNow(ch);
      settleAt(ch, landed);
      ESP_LOGI(TAG, "%s: stopped at %d%%", chName(ch), landed);
    }
  }
}

/**
 * *******************************************************************
 * @brief   drive a shutter to an absolute position
 * @param   channel, targetPct
 * @return  true if the movement was accepted
 * *******************************************************************/
bool shutterPosSetTarget(uint8_t channel, uint8_t targetPct) {

  if (channel >= CH_COUNT) {
    return false;
  }
  if (!shutterPosIsCalibrated(channel)) {
    ESP_LOGW(TAG, "%s: no travel time configured - position command ignored", chName(channel));
    return false;
  }
  if (targetPct > 100) {
    targetPct = 100;
  }

  s_shutterState &st = shutter[channel];

  if (st.moving) {
    st.pos = estimatePos(channel);
  }

  // Nowhere to go. An end-stop request is still worth sending even when the
  // estimate already says we are there, because it is the only way to
  // resynchronise an estimate that has drifted.
  if (st.pos == targetPct && !st.moving && targetPct != 0 && targetPct != 100) {
    return true;
  }

  // A position that is not known yet can only be resolved by running to an
  // end-stop, so anything in between is refused rather than guessed.
  if (st.pos == SHUTTER_POS_UNKNOWN && targetPct != 0 && targetPct != 100) {
    ESP_LOGW(TAG, "%s: position unknown - drive fully open or closed first", chName(channel));
    return false;
  }

  bool needsDown = (st.pos == SHUTTER_POS_UNKNOWN) ? (targetPct == 0) : (targetPct < st.pos);

  // The end-stops are driven without a timed stop: the motor's own limit switch
  // is more accurate than any estimate, and arriving there re-anchors the
  // estimate for everything that follows.
  st.target = (targetPct == 0 || targetPct == 100) ? -1 : (int8_t)targetPct;

  // Already running the right way: no new telegram, just move the deadline.
  // Re-issuing UP or DOWN here would restart the motor and lose the elapsed
  // travel that the current estimate is built on.
  if (st.moving && st.goingDown == needsDown) {
    if (st.target < 0) {
      st.stopAtMs = 0;
    } else {
      int32_t span = needsDown ? (st.pos - st.target) : (st.target - st.pos);
      if (span <= 0) {
        st.stopAtMs = millis();
      } else {
        st.posAtStart = st.pos;
        st.startMs = millis();
        st.stopAtMs = st.startMs + ((uint32_t)span * travelTime(channel, needsDown) / 100u);
      }
    }
    st.pendingMove = false;
    ESP_LOGI(TAG, "%s: retargeted to %d%% without restarting the motor", chName(channel), targetPct);
    return true;
  }

  // Direction change: stop first, otherwise the receiver ignores the reversal.
  if (st.moving) {
    st.stopAtMs = 0;
    jaroStopNow(channel);
    st.pos = estimatePos(channel);
    st.moving = false;
  }

  st.pendingMove = true;
  st.goingDown = needsDown;
  jaroCmd(needsDown ? CMD_DOWN : CMD_UP, channel);
  ESP_LOGI(TAG, "%s: queued %s towards %d%%", chName(channel), needsDown ? "DOWN" : "UP", targetPct);
  return true;
}

/**
 * *******************************************************************
 * @brief   the RF telegram for this channel has just been transmitted
 * @param   channel
 * @return  none
 * *******************************************************************/
void shutterPosNotifyUp(uint8_t channel) {
  if (channel < CH_COUNT) {
    startMovement(channel, false);
  }
}

void shutterPosNotifyDown(uint8_t channel) {
  if (channel < CH_COUNT) {
    startMovement(channel, true);
  }
}

void shutterPosNotifyStop(uint8_t channel) {
  if (channel >= CH_COUNT) {
    return;
  }
  s_shutterState &st = shutter[channel];
  if (!st.moving) {
    return;
  }
  settleAt(channel, estimatePos(channel));
  ESP_LOGI(TAG, "%s: stopped at an estimated %d%%", chName(channel), st.pos);
}

/**
 * *******************************************************************
 * @brief   accessors
 * *******************************************************************/
int8_t shutterPosGet(uint8_t channel) { return (channel < CH_COUNT) ? estimatePos(channel) : SHUTTER_POS_UNKNOWN; }

bool shutterPosIsCalibrated(uint8_t channel) { return (channel < CH_COUNT) && (config.jaro.ch_travel_down[channel] > 0); }

bool shutterPosIsMoving(uint8_t channel) { return (channel < CH_COUNT) && shutter[channel].moving; }
