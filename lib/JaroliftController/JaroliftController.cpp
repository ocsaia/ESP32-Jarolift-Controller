// JaroliftController.cpp
#include "JaroliftController.h"

// Definition of the static instance pointer
JaroliftController *JaroliftController::instance_ = nullptr;

JaroliftController::JaroliftController()
    : devCount_(0), nvsHandle_(0), nvsOpen_(false), devCountValid_(false), deviceKeyMSB_(0), deviceKeyLSB_(0), button_(0), discL_(0), discH_(0),
      disc_(0), newSerial_(0), encrypted_(0), pack_(0), pbWrite_(0), rxOverflow_(0), rxSerial_(0), rxHopCode_(0), rxFunction_(0), rxDiscH_(0),
      initOK_(false), rxDataReady_(false), rxIrqAttached_(false), rxIrqPin_(-1), stopRunSerial_(0), stopRunChannel_(0), stopRunCount_(0),
      stopRunReported_(false), stopRunLastMs_(0), overflowLogMs_(0) {

  memset((void *)lowBuf_, 0, sizeof(lowBuf_));
  memset((void *)hiBuf_, 0, sizeof(hiBuf_));

  uint8_t defaultDiscLow[16] = {0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
  uint8_t defaultDiscHigh[16] = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80};
  memcpy(discLowArr_, defaultDiscLow, sizeof(discLowArr_));
  memcpy(discHighArr_, defaultDiscHigh, sizeof(discHighArr_));

  config_.serial = 0;
  config_.learnMode = true;
  config_.masterMSB = 0;
  config_.masterLSB = 0;

  // Set the Singleton instance
  instance_ = this;
}

JaroliftController::~JaroliftController() {
  // the ISR dereferences instance_, so it has to go before the object does
  detachRxInterrupt();
  if (nvsOpen_) {
    nvs_close(nvsHandle_);
    nvsOpen_ = false;
  }
  instance_ = nullptr;
}

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// helper and setter functions
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

/**
 *******************************************************************
 * @brief   set gpio for CC1101
 * @param   sck, miso, mosi, cs, gd0, gd2
 * @return  none
 * *******************************************************************/
void JaroliftController::setGPIO(int sck, int miso, int mosi, int cs, int gdo0, int gdo2) {
  gpio_.sck = sck;
  gpio_.miso = miso;
  gpio_.mosi = mosi;
  gpio_.cs = cs;
  gpio_.gdo0 = gdo0;
  gpio_.gdo2 = gdo2;
}

/**
 *******************************************************************
 * @brief   set serial number (6 of 8 bytes)
 * @param   serial
 * @return  none
 * *******************************************************************/
void JaroliftController::setBaseSerial(uint32_t serial) {
  config_.serial = serial;
  ESP_LOGI(TAG, "Set base serial: 0x%08lx", config_.serial);
}

/**
 *******************************************************************
 * @brief   get serial for given channel
 * @param   channel
 * @return  none
 * *******************************************************************/
uint32_t JaroliftController::getSerial(uint8_t channel) {
  uint32_t serial = (config_.serial << 8) | channel;
  ESP_LOGD(TAG, "serial: 0x%08lx | channel: %d", serial, channel + 1);
  return serial;
}

/**
 *******************************************************************
 * @brief   check that a channel can be used as an index into the disc arrays
 * @details B3: callers derive the channel as "number - 1", so a command without
 *          a channel parameter arrives here as 255 and used to read
 *          discLowArr_/discHighArr_ far behind their 16 entries. The library has
 *          to defend itself because it is reached from telnet, MQTT and the
 *          WebUI, and each of them validates differently or not at all.
 * @param   channel, cmdName (used for the log line only)
 * @return  true if the channel may be used
 * *******************************************************************/
bool JaroliftController::channelValid(uint8_t channel, const char *cmdName) const {
  if (channel < kMaxChannels) {
    return true;
  }
  ESP_LOGE(TAG, "%s: invalid channel %u - must be 0...%u", cmdName, (unsigned)channel, (unsigned)(kMaxChannels - 1));
  return false;
}

/**
 *******************************************************************
 * @brief   set legacy learn mode
 * @param   legacyMode
 * @return  none
 * *******************************************************************/
void JaroliftController::setLegacyLearnMode(bool legacyMode) { config_.learnMode = !legacyMode; }

/**
 *******************************************************************
 * @brief   set jarolift master keys
 * @param   masterMSB, masterLSB
 * @return  none
 * *******************************************************************/
void JaroliftController::setKeys(unsigned long masterMSB, unsigned long masterLSB) {
  config_.masterMSB = masterMSB;
  config_.masterLSB = masterLSB;
}

/**
 *******************************************************************
 * @brief   get state if CC1101 is connected
 * @param   none
 * @return  none
 * *******************************************************************/
bool JaroliftController::getCC1101State() { return cc1101_.connected(); }

/**
 *******************************************************************
 * @brief   open the NVS namespace that stores the device counter
 * @details D4: the handle used to be opened and closed on every single counter
 *          access. cmdUnlearn() touches the counter nine times, so that was nine
 *          open/close pairs plus nine pointless delay(100) - roughly 800 ms added
 *          to a command that already blocks loop() for ~5 s against a watchdog
 *          that is only fed from loop().
 * @param   none
 * @return  true if the handle is usable
 * *******************************************************************/
bool JaroliftController::openNvs() {
  if (nvsOpen_) {
    return true;
  }
  esp_err_t err = nvs_open("device_data", NVS_READWRITE, &nvsHandle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return false;
  }
  nvsOpen_ = true;
  return true;
}

/**
 *******************************************************************
 * @brief   get the device counter (the KeeLoq rolling counter)
 * @param   none
 * @return  device counter, 0 if NVS is unavailable
 * *******************************************************************/
uint16_t JaroliftController::getDeviceCounter() {
  // the counter is read for every transmitted frame, so it is cached in RAM. NVS
  // stays the master copy: setDeviceCounter() commits every change immediately, so
  // a crash cannot leave the paired receivers ahead of us and ignoring commands.
  if (devCountValid_) {
    return devCount_;
  }
  if (!openNvs()) {
    return 0;
  }
  uint16_t counter = 0;
  esp_err_t err = nvs_get_u16(nvsHandle_, "devcnt", &counter);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    // try to read from EEPROM if not found in NVS (migration)
    EEPROM.get(0, counter);
    nvs_set_u16(nvsHandle_, "devcnt", counter);
    nvs_commit(nvsHandle_);
    ESP_LOGI(TAG, "Migrated devcnt=%d from EEPROM to NVS", counter);
  } else if (err != ESP_OK) {
    // do not cache a value we did not actually read
    ESP_LOGE(TAG, "Failed to read devcnt: %s", esp_err_to_name(err));
    return 0;
  }
  devCount_ = counter;
  devCountValid_ = true;
  if (counter == 0) {
    setDeviceCounter(1);
  }
  return devCount_;
}

/**
 *******************************************************************
 * @brief   set device counter
 * @param   newDevCnt
 * @return  none
 * *******************************************************************/
void JaroliftController::setDeviceCounter(uint16_t newDevCnt) {
  devCount_ = newDevCnt;
  devCountValid_ = true;
  if (!openNvs()) {
    return;
  }
  esp_err_t err = nvs_set_u16(nvsHandle_, "devcnt", devCount_);
  if (err == ESP_OK) {
    // commit on every change: this is the rolling counter, and a stored value that
    // lags behind what the shutters have seen makes them ignore the next commands
    err = nvs_commit(nvsHandle_);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to store devcnt: %s", esp_err_to_name(err));
  }
  // The delay(100) that used to sit here was not needed for the write itself -
  // nvs_commit() has already flushed by the time it returns. It was, however,
  // load-bearing over the air: in the service sequences this function is called
  // between two transmissions, so it formed part of the inter-frame gap that the
  // receiver's programming state machine sees. Those sequences now wait 400 ms
  // instead of 300 ms so the observable spacing is unchanged; only the wasted
  // nvs_open/close pairs and this delay are gone.
}

/**
 *******************************************************************
 * @brief   update and increment device counter
 * @param   increment
 * @return  none
 * *******************************************************************/
void JaroliftController::updateDeviceCounter(bool increment) {
  devCount_ = getDeviceCounter();
  if (increment) {
    devCount_++;
    setDeviceCounter(devCount_);
  }
}

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// CC1101 radio functions group
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

/**
 *******************************************************************
 * @brief   Generates sync-pulses (protocoll start)
 * @param   length
 * @return  none
 * *******************************************************************/
void JaroliftController::radioTxFrame(int length) {
  for (int i = 0; i < length; ++i) {
    digitalWrite(gpio_.gdo0, LOW);
    delayMicroseconds(400);
    digitalWrite(gpio_.gdo0, HIGH);
    delayMicroseconds(380);
  }
}

/**
 *******************************************************************
 * @brief   Sending of high_group_bits 8-16 (discH_)
 * @param   none
 * @return  none
 * *******************************************************************/
void JaroliftController::radioTxGroupH() {
  for (int i = 0; i < 8; i++) {
    int bitVal = (discH_ >> i) & 0x1;
    if (bitVal == 1) {
      digitalWrite(gpio_.gdo0, LOW);
      delayMicroseconds(kLowPulse);
      digitalWrite(gpio_.gdo0, HIGH);
      delayMicroseconds(kHighPulse);
    } else {
      digitalWrite(gpio_.gdo0, LOW);
      delayMicroseconds(kHighPulse);
      digitalWrite(gpio_.gdo0, HIGH);
      delayMicroseconds(kLowPulse);
    }
  }
}

/**
 *******************************************************************
 * @brief   tx routine to send data
 * @param   repetitions
 * @return  none
 * *******************************************************************/
void JaroliftController::radioTx(int repetitions) {
  pack_ = (button_ << 60) | (newSerial_ << 32) | encrypted_;
  for (int a = 0; a < repetitions; a++) {
    digitalWrite(gpio_.gdo0, LOW);
    delayMicroseconds(1150);
    radioTxFrame(13);
    delayMicroseconds(3500);
    for (int i = 0; i < 64; i++) {
      int bitVal = (pack_ >> i) & 0x1;
      if (bitVal == 1) {
        digitalWrite(gpio_.gdo0, LOW);
        delayMicroseconds(kLowPulse);
        digitalWrite(gpio_.gdo0, HIGH);
        delayMicroseconds(kHighPulse);
      } else {
        digitalWrite(gpio_.gdo0, LOW);
        delayMicroseconds(kHighPulse);
        digitalWrite(gpio_.gdo0, HIGH);
        delayMicroseconds(kLowPulse);
      }
    }
    radioTxGroupH();
    delay(16);
  }
}

/**
 *******************************************************************
 * @brief   arm the RX interrupt on the gdo2 pin
 * @details D3: the handler used to be attached in begin() and then attached again
 *          on every received frame, while detachInterrupt() appeared nowhere in
 *          the codebase. Going through this pair keeps attach and detach
 *          symmetric, so enterTx() can switch the ISR off for the duration of a
 *          transmission and enterRx() switches it back on.
 * @param   none
 * @return  none
 * *******************************************************************/
void JaroliftController::attachRxInterrupt() {
  if (rxIrqAttached_) {
    return;
  }
  // The Arduino core guards attachInterrupt() against pin >= SOC_GPIO_PIN_COUNT
  // but __detachInterrupt() has no such check and indexes __pinInterruptHandlers[]
  // unguarded, so releasing an out-of-range pin corrupts core memory. Refusing to
  // record an invalid pin as armed is what keeps the detach side safe, since the
  // default gdo2 of 22 does not exist on every supported target.
  if (!digitalPinIsValid(gpio_.gdo2)) {
    ESP_LOGE(TAG, "gdo2 (%d) is not a valid GPIO - RX interrupt not armed", gpio_.gdo2);
    return;
  }
  // whatever is in the buffer predates the pause, and the ISR statics still hold
  // the edge timestamps from before it - start the next frame from a clean state
  portENTER_CRITICAL(&rxMux_);
  pbWrite_ = 0;
  lowBuf_[0] = 0;
  portEXIT_CRITICAL(&rxMux_);
  rxIrqPin_ = gpio_.gdo2;
  attachInterrupt(rxIrqPin_, radioRxMeasureISR, CHANGE);
  rxIrqAttached_ = true;
}

/**
 *******************************************************************
 * @brief   disarm the RX interrupt
 * @param   none
 * @return  none
 * *******************************************************************/
void JaroliftController::detachRxInterrupt() {
  if (!rxIrqAttached_) {
    return;
  }
  // deliberately not gpio_.gdo2: the configured pin may have been changed since
  // the interrupt was armed, and releasing the wrong one would leave this one live
  if (digitalPinIsValid(rxIrqPin_)) {
    detachInterrupt(rxIrqPin_);
  }
  rxIrqAttached_ = false;
}

/**
 *******************************************************************
 * @brief   put CC1101 to receive mode
 * @param   none
 * @return  none
 * *******************************************************************/
void JaroliftController::enterRx() {
  cc1101_.setRxState();
  delay(2);
  unsigned long startTime = micros();
  uint8_t marcState = 0;
  while (((marcState = cc1101_.readStatusReg(CC1101_MARCSTATE)) & 0x1F) != 0x0D) {
    if (micros() - startTime > 50000)
      break;
  }
  // arm the ISR only once the receiver really is in RX, so the edges the CC1101
  // produces while it settles are not measured as pulses of a frame
  attachRxInterrupt();
}

/**
 *******************************************************************
 * @brief   put CC1101 to send mode
 * @param   none
 * @return  none
 * *******************************************************************/
void JaroliftController::enterTx() {
  // D3: radioTx() bit-bangs the frame with delayMicroseconds(). An RX interrupt
  // firing in the middle of that stretches the pulse currently being transmitted
  // by the runtime of the handler. Nothing can be received while the CC1101 sends
  // anyway, and every transmit path in this library ends in enterRx(), which arms
  // the ISR again.
  detachRxInterrupt();
  cc1101_.setTxState();
  delay(2);
  unsigned long startTime = micros();
  uint8_t marcState = 0;
  while (((marcState = cc1101_.readStatusReg(CC1101_MARCSTATE)) & 0x1F) != 0x13 && ((marcState & 0x1F) != 0x14) && ((marcState & 0x1F) != 0x15)) {
    if (micros() - startTime > 50000)
      break;
  }
}

/**
 *******************************************************************
 * @brief   calculate RSSI value (Received Signal Strength Indicator)
 * @param   none
 * @return  none
 * *******************************************************************/
uint8_t JaroliftController::getRssi() {
  uint8_t rssi = cc1101_.readReg(CC1101_RSSI, CC1101_STATUS_REGISTER);
  uint8_t value = 0;
  if (rssi >= 128) {
    value = 255 - rssi;
    value = value / 2;
    value = value + 74;
  } else {
    value = rssi / 2;
    value = value + 74;
  }
  return value;
}

/**
 *******************************************************************
 * @brief   Calculate device keys based on serial and master keys
 * @param   none
 * @return  none
 * *******************************************************************/
void JaroliftController::generateKey() {
  Keeloq k(config_.masterMSB, config_.masterLSB);
  uint64_t keyInput = newSerial_ | 0x20000000;
  unsigned long enc = k.decrypt(keyInput);
  deviceKeyLSB_ = enc;
  keyInput = newSerial_ | 0x60000000;
  enc = k.decrypt(keyInput);
  deviceKeyMSB_ = enc;
}

/**
 *******************************************************************
 * @brief   Generation of the encrypted message (Hopcode)
 * @param   none
 * @return  none
 * *******************************************************************/
void JaroliftController::generateEncrypted() {
  Keeloq k(deviceKeyMSB_, deviceKeyLSB_);
  devCount_ = getDeviceCounter();
  unsigned int result = (disc_ << 16) | devCount_; // Append counter value to discrimination value
  encrypted_ = k.encrypt(result);
}

/**
 *******************************************************************
 * @brief   encrypt device keys from received hopcode based on received Serialnumber
 * @details Here normal key-generation is used according to 00745a_c.PDF Appendix G.
 * @details https://github.com/hnhkj/documents/blob/master/KEELOQ/docs/AN745/00745a_c.pdf
 * @param   none
 * @return  none
 * *******************************************************************/
void JaroliftController::rxKeyGen() {
  Keeloq k(config_.masterMSB, config_.masterLSB);
  uint32_t keylow = rxSerial_ | 0x20000000;
  unsigned long enc = k.decrypt(keylow);
  deviceKeyLSB_ = enc; // Stores LSB devicekey 16Bit
  keylow = rxSerial_ | 0x60000000;
  enc = k.decrypt(keylow);
  deviceKeyMSB_ = enc; // Stores MSB devicekey 16Bit
}

/**
 *******************************************************************
 * @brief   encrypt received hopcode based calculated device keys
 * @param   none
 * @return  none
 * *******************************************************************/
uint32_t JaroliftController::rxDecode() {
  Keeloq k(deviceKeyMSB_, deviceKeyLSB_);
  unsigned int result = rxHopCode_;
  return k.decrypt(result);
}

/**
 *******************************************************************
 * @brief   Interrupt-Service-Routine (ISR)
 * @param   none
 * @return  none
 * *******************************************************************/
void IRAM_ATTR JaroliftController::radioRxMeasureISR() {
  if (instance_) {
    instance_->handleRadioRxMeasure();
  }
}

/**
 *******************************************************************
 * @brief   Handling incoming data
 * @param   none
 * @return  none
 * *******************************************************************/
void JaroliftController::handleRadioRxMeasure() {
  static unsigned long lineUp = 0;
  static unsigned long lineDown = 0;
  static unsigned long timeout = 0;
  unsigned long currentMicros = micros();
  int pinState = digitalRead(gpio_.gdo2);

  // Everything below touches state that processRxData() reads, so it runs under
  // the spinlock. micros() and digitalRead() stay outside to keep the masked
  // window down to a few hundred nanoseconds per edge.
  portENTER_CRITICAL_ISR(&rxMux_);

  if (currentMicros - timeout > 3500) {
    pbWrite_ = 0;
  }
  if (pinState) { // Übergang zu HIGH
    lineUp = currentMicros;
    unsigned long lowVal = lineUp - lineDown;
    if (lowVal >= kDebounce && lowVal > 300 && lowVal < 4300) {
      if (lowVal > 3650) {
        // sync pulse - a frame always starts at index 0
        timeout = currentMicros;
        pbWrite_ = 0;
        lowBuf_[pbWrite_] = (uint16_t)lowVal;
        pbWrite_++;
      } else if (lowVal < 1000) {
        // A1: pbWrite_ used to grow without any bound. A 433 MHz burst with more
        // than kPulseBufferSize pulses in the accepted range wrote straight past
        // both buffers into pbWrite_, rxSerial_, initOK_ and cc1101_ - the shared
        // ISM band makes that a matter of time. A frame is ~73 pulses, so a burst
        // this long is never one: throw it away and resynchronise on the next
        // sync pulse.
        if (pbWrite_ >= kPulseBufferSize) {
          pbWrite_ = 0;
          rxOverflow_++;
        }
        lowBuf_[pbWrite_] = (uint16_t)lowVal;
        pbWrite_++;
        timeout = currentMicros;
      }
    }
  } else { // Übergang zu LOW
    lineDown = currentMicros;
    unsigned long highVal = lineDown - lineUp;
    // the second half of A1: after the increment above pbWrite_ can be exactly
    // kPulseBufferSize, and this is the branch that writes at that index
    if (highVal >= kDebounce && highVal > 300 && highVal < 1000 && pbWrite_ < kPulseBufferSize) {
      hiBuf_[pbWrite_] = (uint16_t)highVal;
    }
  }

  portEXIT_CRITICAL_ISR(&rxMux_);
}

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// Shutter command functions
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

/**
 * *******************************************************************
 * @brief   send channel commands (Up, DOWN, STOP, SHADE)
 * @param   cmd, channel
 * @return  none
 * *******************************************************************/
void JaroliftController::cmdChannel(commands cmd, uint8_t channel) {
  // B3: reached from telnet with "channel - 1" == 255 when no channel was typed,
  // which used to index discLowArr_/discHighArr_[16] 239 entries out of bounds
  if (!initOK_ || !channelValid(channel, "cmdChannel"))
    return;
  newSerial_ = getSerial(channel);

  switch (cmd) {
  case CMD_UP:
    button_ = FCT_CODE_UP;
    discL_ = discLowArr_[channel];
    discH_ = discHighArr_[channel];
    disc_ = (discL_ << 8) | (newSerial_ & 0xFF);
    generateKey();
    generateEncrypted();
    enterTx();
    radioTx(2); // send command 2-times with same devCnt
    enterRx();
    updateDeviceCounter(true);
    break;

  case CMD_DOWN:
    button_ = FCT_CODE_DOWN;
    discL_ = discLowArr_[channel];
    discH_ = discHighArr_[channel];
    disc_ = (discL_ << 8) | (newSerial_ & 0xFF);
    generateKey();
    generateEncrypted();
    enterTx();
    radioTx(2); // send command 2-times with same devCnt
    enterRx();
    updateDeviceCounter(true);
    break;

  case CMD_STOP:
    button_ = FCT_CODE_STOP;
    discL_ = discLowArr_[channel];
    discH_ = discHighArr_[channel];
    disc_ = (discL_ << 8) | (newSerial_ & 0xFF);
    generateKey();
    generateEncrypted();
    enterTx();
    radioTx(2); // send command 2-times
    enterRx();
    updateDeviceCounter(true);
    break;

  case CMD_SHADE:
    button_ = FCT_CODE_STOP;
    discL_ = discLowArr_[channel];
    discH_ = discHighArr_[channel];
    disc_ = (discL_ << 8) | (newSerial_ & 0xFF);
    generateKey();
    generateEncrypted();
    enterTx();
    radioTx(20); // send "continuos STOP"
    enterRx();
    updateDeviceCounter(true);
    break;

  case CMD_SET_SHADE:
    button_ = FCT_CODE_STOP;
    discL_ = discLowArr_[channel];
    discH_ = discHighArr_[channel];
    disc_ = (discL_ << 8) | (newSerial_ & 0xFF);
    generateKey();
    // send 4-times STOP
    for (int i = 0; i < 4; i++) {
      enterTx();
      generateEncrypted();
      radioTx(1);
      updateDeviceCounter(true);
      enterRx();
      delay(400);
    }
    break;

  default:
    break;
  }
}

/**
 * *******************************************************************
 * @brief   send group commands (Up, DOWN, STOP, SHADE)
 * @param   cmd, channel
 * @return  none
 * *******************************************************************/
void JaroliftController::cmdGroup(commands cmd, uint16_t groupMask) {
  if (!initOK_)
    return;
  // B3, group half: this function takes a bit mask, not a channel index, so there
  // is no array access to bound here - the out-of-bounds read the plan describes
  // is config.jaro.grp_mask[-1] on the telnet side and has to be fixed there. What
  // the library can reject is an empty mask, which addresses no shutter at all and
  // would only burn a rolling-counter step.
  if (groupMask == 0) {
    ESP_LOGE(TAG, "cmdGroup: empty group mask - nothing to send");
    return;
  }
  devCount_ = getDeviceCounter();
  newSerial_ = getSerial(0);

  switch (cmd) {
  case CMD_UP:
    button_ = FCT_CODE_UP;
    break;

  case CMD_DOWN:
    button_ = FCT_CODE_DOWN;
    break;

  case CMD_STOP:
  case CMD_SHADE:
    button_ = FCT_CODE_STOP;
    break;

  default:
    break;
  }

  discL_ = groupMask & 0x00FF;
  discH_ = (groupMask >> 8) & 0x00FF;
  disc_ = (discL_ << 8) | (newSerial_ & 0xFF);
  generateKey();
  generateEncrypted();
  enterTx();
  if (cmd == CMD_SHADE) {
    radioTx(20); // send "continuos STOP"
  } else {
    radioTx(2); // send command 2-times
  }

  enterRx();
  updateDeviceCounter(true);
}

/**
 * *******************************************************************
 * @brief   send command to learn a new shutter
 * @param   channel
 * @return  none
 * *******************************************************************/
void JaroliftController::cmdLearn(uint8_t channel) {
  if (!initOK_ || !channelValid(channel, "cmdLearn"))
    return;
  newSerial_ = getSerial(channel);
  devCount_ = getDeviceCounter();
  ESP_LOGD(TAG, "learn | Device Counter: %d | Serial: 0x%08llx", devCount_, newSerial_);
  button_ = config_.learnMode ? 0xA : 0x1;
  discL_ = discLowArr_[channel];
  discH_ = discHighArr_[channel];
  disc_ = (discL_ << 8) | (newSerial_ & 0xFF);
  generateKey();
  generateEncrypted();
  enterTx();
  radioTx(2);
  enterRx();
  updateDeviceCounter(true);
  if (config_.learnMode) {
    delay(1000);
    button_ = 0x4; // Stop
    generateEncrypted();
    enterTx();
    radioTx(2);
    enterRx();
    updateDeviceCounter(true);
  }
}

/**
 * *******************************************************************
 * @brief   send command to unlearn a existing shutter
 * @details UP/DOWN -> 6x STOP -> UP
 * @param   channel
 * @return  none
 * *******************************************************************/
void JaroliftController::cmdUnlearn(uint8_t channel) {
  if (!initOK_ || !channelValid(channel, "cmdUnlearn"))
    return;
  newSerial_ = getSerial(channel);
  devCount_ = getDeviceCounter();
  ESP_LOGD(TAG, "unlearn | Device Counter: %d | Serial: 0x%08llx", devCount_, newSerial_);
  discL_ = discLowArr_[channel];
  discH_ = discHighArr_[channel];
  disc_ = (discL_ << 8) | (newSerial_ & 0xFF);
  generateKey();
  generateEncrypted();
  enterTx();
  button_ = FCT_CODE_UPDOWN; // Up+Down
  radioTx(2);
  enterRx();
  updateDeviceCounter(true);
  delay(400);
  for (int i = 0; i < 6; i++) {
    button_ = FCT_CODE_STOP; // 6x Stop
    enterTx();
    generateEncrypted();
    radioTx(2);
    updateDeviceCounter(true);
    enterRx();
    delay(400);
  }
  button_ = FCT_CODE_UP; // UP
  enterTx();
  generateEncrypted();
  radioTx(2);
  updateDeviceCounter(true);
  enterRx();
}

/**
 * *******************************************************************
 * @brief   send command to set the upper end point
 * @details UP/DOWN -> 2x STOP -> UP
 * @param   channel
 * @return  none
 * *******************************************************************/
void JaroliftController::cmdSetEndPointUp(uint8_t channel) {
  if (!initOK_ || !channelValid(channel, "cmdSetEndPointUp"))
    return;
  newSerial_ = getSerial(channel);
  devCount_ = getDeviceCounter();
  ESP_LOGD(TAG, "set upper end point | Device Counter: %d | Serial: 0x%08llx", devCount_, newSerial_);
  discL_ = discLowArr_[channel];
  discH_ = discHighArr_[channel];
  disc_ = (discL_ << 8) | (newSerial_ & 0xFF);
  generateKey();
  generateEncrypted();
  enterTx();
  button_ = FCT_CODE_UPDOWN; // Up+Down
  radioTx(1);
  enterRx();
  updateDeviceCounter(true);
  delay(400);
  for (int i = 0; i < 2; i++) {
    button_ = FCT_CODE_STOP; // 2x Stop
    enterTx();
    generateEncrypted();
    radioTx(1);
    updateDeviceCounter(true);
    enterRx();
    delay(400);
  }
  button_ = FCT_CODE_UP; // UP
  enterTx();
  generateEncrypted();
  radioTx(1);
  updateDeviceCounter(true);
  enterRx();
}

/**
 * *******************************************************************
 * @brief   send command to delete the upper end point
 * @details UP/DOWN -> 2x STOP -> UP
 * @param   channel
 * @return  none
 * *******************************************************************/
void JaroliftController::cmdDeleteEndPointUp(uint8_t channel) {
  if (!initOK_ || !channelValid(channel, "cmdDeleteEndPointUp"))
    return;
  newSerial_ = getSerial(channel);
  devCount_ = getDeviceCounter();
  ESP_LOGD(TAG, "delete upper end point | Device Counter: %d | Serial: 0x%08llx", devCount_, newSerial_);
  discL_ = discLowArr_[channel];
  discH_ = discHighArr_[channel];
  disc_ = (discL_ << 8) | (newSerial_ & 0xFF);
  generateKey();
  generateEncrypted();
  enterTx();
  button_ = FCT_CODE_UPDOWN; // Up+Down
  radioTx(1);
  enterRx();
  updateDeviceCounter(true);
  delay(400);
  for (int i = 0; i < 4; i++) {
    button_ = FCT_CODE_STOP; // 4x Stop
    enterTx();
    generateEncrypted();
    radioTx(1);
    updateDeviceCounter(true);
    enterRx();
    delay(400);
  }
  button_ = FCT_CODE_UP; // UP
  enterTx();
  generateEncrypted();
  radioTx(1);
  updateDeviceCounter(true);
  enterRx();
}

/**
 * *******************************************************************
 * @brief   send command to set the lower end point
 * @details UP/DOWN -> 2x STOP -> DOWN
 * @param   channel
 * @return  none
 * *******************************************************************/
void JaroliftController::cmdSetEndPointDown(uint8_t channel) {
  if (!initOK_ || !channelValid(channel, "cmdSetEndPointDown"))
    return;
  newSerial_ = getSerial(channel);
  devCount_ = getDeviceCounter();
  ESP_LOGD(TAG, "set lower end point | Device Counter: %d | Serial: 0x%08llx", devCount_, newSerial_);
  discL_ = discLowArr_[channel];
  discH_ = discHighArr_[channel];
  disc_ = (discL_ << 8) | (newSerial_ & 0xFF);
  generateKey();
  generateEncrypted();
  enterTx();
  button_ = FCT_CODE_UPDOWN; // Up+Down
  radioTx(1);
  enterRx();
  updateDeviceCounter(true);
  delay(400);
  for (int i = 0; i < 2; i++) {
    button_ = FCT_CODE_STOP; // 2x Stop
    enterTx();
    generateEncrypted();
    radioTx(1);
    updateDeviceCounter(true);
    enterRx();
    delay(400);
  }
  button_ = FCT_CODE_DOWN; // DOWN
  enterTx();
  generateEncrypted();
  radioTx(1);
  updateDeviceCounter(true);
  enterRx();
}

/**
 * *******************************************************************
 * @brief   send command to delete the lower end point
 * @details UP/DOWN -> 2x STOP -> UP
 * @param   channel
 * @return  none
 * *******************************************************************/
void JaroliftController::cmdDeleteEndPointDown(uint8_t channel) {
  if (!initOK_ || !channelValid(channel, "cmdDeleteEndPointDown"))
    return;
  newSerial_ = getSerial(channel);
  devCount_ = getDeviceCounter();
  ESP_LOGD(TAG, "delete lower end point | Device Counter: %d | Serial: 0x%08llx", devCount_, newSerial_);
  discL_ = discLowArr_[channel];
  discH_ = discHighArr_[channel];
  disc_ = (discL_ << 8) | (newSerial_ & 0xFF);
  generateKey();
  generateEncrypted();
  enterTx();
  button_ = FCT_CODE_UPDOWN; // Up+Down
  radioTx(1);
  enterRx();
  updateDeviceCounter(true);
  delay(400);
  for (int i = 0; i < 4; i++) {
    button_ = FCT_CODE_STOP; // 4x Stop
    enterTx();
    generateEncrypted();
    radioTx(1);
    updateDeviceCounter(true);
    enterRx();
    delay(400);
  }
  button_ = FCT_CODE_DOWN; // DOWN
  enterTx();
  generateEncrypted();
  radioTx(1);
  updateDeviceCounter(true);
  enterRx();
}

/**
 * *******************************************************************
 * @brief   process received data
 * @param   none
 * @return  none
 * *******************************************************************/
void JaroliftController::processRxData() {

  // A1: the ISR appends to lowBuf_/hiBuf_ on every gdo2 edge, so the frame has to
  // be taken out as one consistent snapshot. Decoding in place mixes the pulses of
  // two frames, and pbWrite_ can change between the check and the last bit read.
  //
  // A portMUX spinlock is the right primitive here. It also serialises against an
  // ISR running on the other core (masking interrupts locally would not), it masks
  // interrupts for ~2 µs and only when a frame is actually taken out - a pulse is
  // 300-1000 µs wide and a GPIO edge is latched in hardware, so an edge is delayed
  // by about a microsecond rather than lost - and it costs the ISR a handful of
  // instructions per edge. An ISR-owned double buffer was rejected because the ISR
  // has no way to know where a frame ends: completeness is decided right here from
  // the pulse count, so the ISR could only hand over on the next sync pulse and an
  // isolated frame would stay undecoded until unrelated traffic arrived.
  // detachInterrupt() was rejected because it tears the GPIO handler down and
  // rebuilds it on every loop() pass and is deaf for the whole window.
  bool frameComplete = false;
  unsigned int pulseCount = 0;

  portENTER_CRITICAL(&rxMux_);
  pulseCount = pbWrite_;
  // check if RX-Buffer is full and start to decode
  frameComplete = (lowBuf_[0] > 3650 && lowBuf_[0] < 4300) && (pulseCount >= 65 && pulseCount <= 75);
  if (frameComplete) {
    memcpy(snapLow_, (const void *)lowBuf_, sizeof(snapLow_));
    memcpy(snapHi_, (const void *)hiBuf_, sizeof(snapHi_));
    // consume the frame while the ISR is still locked out, otherwise a pulse that
    // arrives between the copy and the reset is silently prepended to the next one
    pbWrite_ = 0;
    memset((void *)lowBuf_, 0, sizeof(lowBuf_));
    memset((void *)hiBuf_, 0, sizeof(hiBuf_));
  }
  portEXIT_CRITICAL(&rxMux_);

  if (!frameComplete) {
    return;
  }

  rxDataReady_ = true;
  ESP_LOGD(TAG, "frame received | pulses: %u", pulseCount);

  // extract Hopcode (32 Bit)
  rxHopCode_ = 0;
  for (int i = 0; i < 32; i++) {
    if (snapLow_[i + 1] < snapHi_[i + 1])
      rxHopCode_ &= ~(1 << i);
    else
      rxHopCode_ |= (1 << i);
  }

  // extract Serial (28 Bit)
  rxSerial_ = 0;
  for (int i = 0; i < 28; i++) {
    if (snapLow_[i + 33] < snapHi_[i + 33])
      rxSerial_ &= ~(1 << i);
    else
      rxSerial_ |= (1 << i);
  }

  // extract function code (4 Bit)
  rxFunction_ = 0;
  for (int i = 0; i < 4; i++) {
    if (snapLow_[61 + i] < snapHi_[61 + i])
      rxFunction_ &= ~(1 << i);
    else
      rxFunction_ |= (1 << i);
  }
  // extract high disc - group bits (9-16 Bit)
  rxDiscH_ = 0;
  for (int i = 0; i < 8; i++) {
    if (snapLow_[65 + i] < snapHi_[65 + i])
      rxDiscH_ &= ~(1 << i);
    else
      rxDiscH_ |= (1 << i);
  }

  rxKeyGen();
  uint32_t decoded = rxDecode();

  // build channel information
  uint8_t ch_low = (decoded >> 24) & 0xFF;
  uint8_t ch_high = rxDiscH_ & 0xFF;
  uint16_t channel = (ch_high << 8) | ch_low;

  // D1: a remote sends SHADE as a long press on STOP, which arrives as a run of
  // STOP frames from the same remote for the same channels. steadyCount_ tried to
  // count that with one unsigned int shared by every remote: the first non-STOP
  // frame decremented it from 0 to 0xFFFFFFFF, which is outside the
  // "> 10 && <= 40" window, so SHADE detection stayed dead until twelve further
  // STOP frames wrapped it back to 11. It also never expired, so STOP presses
  // hours apart eventually added up to a SHADE nobody asked for. The run is now
  // per remote, per channel, and dies after kShadeRunGapMs.
  if (rxFunction_ == FCT_CODE_STOP) {
    unsigned long now = millis();
    if (rxSerial_ != stopRunSerial_ || channel != stopRunChannel_ || (now - stopRunLastMs_) > kShadeRunGapMs) {
      stopRunSerial_ = rxSerial_;
      stopRunChannel_ = channel;
      stopRunCount_ = 0;
      stopRunReported_ = false;
    }
    stopRunLastMs_ = now;
    if (stopRunCount_ < 0xFF) {
      stopRunCount_++;
    }
    if (stopRunCount_ >= kShadeStopFrames && !stopRunReported_) {
      // report the long press exactly once - holding the button even longer must
      // not send a second SHADE, so the rest of the run stays STOP
      stopRunReported_ = true;
      rxFunction_ = FCT_CODE_SHADE;
      ESP_LOGD(TAG, "long STOP press -> SHADE | serial: 0x%08lx | frames: %u", (unsigned long)rxSerial_, (unsigned)stopRunCount_);
    }
  } else {
    // any other function code ends the run
    stopRunSerial_ = rxSerial_;
    stopRunChannel_ = channel;
    stopRunCount_ = 0;
    stopRunReported_ = false;
    stopRunLastMs_ = millis();
  }

  // callback function to receive information outside this library
  // D2: begin() arms the RX interrupt before the application gets a chance to call
  // setRemoteCallback(), so a frame can be decoded while this is still null
  if (remoteCallback != nullptr) {
    remoteCallback(rxSerial_, rxFunction_, channel);
  } else {
    ESP_LOGD(TAG, "remote frame decoded but no callback registered | serial: 0x%08lx", (unsigned long)rxSerial_);
  }

  // reset variables
  rxDiscH_ = 0;
  rxHopCode_ = 0;
  rxFunction_ = 0;
}

/**
 * *******************************************************************
 * @brief   Setup function for jarolift controller
 * @param   none
 * @return  none
 * *******************************************************************/
void JaroliftController::begin() {
  ESP_LOGI(TAG, "start CC1101 setup");

  // Take any previously armed interrupt down first and treat the controller as
  // uninitialised until the radio is back up. begin() runs once from setup(); the
  // WebUI no longer re-enters it, because key/serial/learn-mode edits go through
  // jaroApplyRadioConfig(), which only stores the values.
  detachRxInterrupt();
  initOK_ = false;

  EEPROM.begin(sizeof(devCount_));
  devCount_ = getDeviceCounter();

  cc1101_.setGPIO(gpio_.sck, gpio_.miso, gpio_.mosi, gpio_.cs, gpio_.gdo0);
  if (!cc1101_.init()) {
    // initOK_ stays false: every command path and loop() bail out on it, so the
    // controller is inert rather than transmitting with an unconfigured radio.
    ESP_LOGE(TAG, "Initialisation of the CC1101 module aborted - radio disabled!");
    return;
  }
  cc1101_.setSyncWord(kSyncWord, false);
  cc1101_.setCarrierFreq(CFREQ_433);
  cc1101_.disableAddressCheck();
  cc1101_.setTxPowerAmp(PA_LongDistance);

  pinMode(gpio_.gdo0, OUTPUT);
  pinMode(gpio_.gdo2, INPUT_PULLUP);

  // init() leaves the CC1101 in IDLE, so arming the interrupt is not enough:
  // without this the receiver stays deaf until the first transmission happens to
  // enter RX as a side effect at the end of cmdChannel().
  // enterRx() now also attaches the RX interrupt (D3), which is why there is no
  // separate attachInterrupt() here any more - enterTx() takes it down again for
  // the duration of every transmission.
  enterRx();

  initOK_ = true;
}

/**
 * *******************************************************************
 * @brief   cyclic process of jarolift controller
 * @param   none
 * @return  none
 * *******************************************************************/
void JaroliftController::loop() {
  if (!initOK_)
    return;

  if (rxDataReady_) {
    // D3: recalibrate the receiver after a decoded frame with the ISR disarmed.
    // The re-attachInterrupt() that used to sit at the end of this block was a
    // no-op - nothing had ever detached - so every edge the CC1101 produced while
    // it was recalibrated and re-tuned was measured as if it were part of a frame.
    detachRxInterrupt();
    cc1101_.cmdStrobe(CC1101_SCAL);
    delay(50);
    rxDataReady_ = false;
    // enterRx() before the settle, not after: the ISR only has to be down across
    // the recalibration itself. Arming it afterwards would leave the receiver deaf
    // for the whole 200 ms too, which is longer than a remote's frame repeat
    // period - the second frame of a button press would be missed.
    enterRx(); // back into RX, and arm the ISR again
    delay(200);
  }

  processRxData(); // process received data

  // A1: report a saturated pulse buffer, but not once per burst - a neighbour's
  // 433 MHz device would otherwise flood the log. Before the clamp in the ISR this
  // condition was not survivable, it silently corrupted the members behind the
  // buffers, so it is worth knowing that it happens at all.
  if (rxOverflow_ != 0 && (millis() - overflowLogMs_) > 60000) {
    uint32_t dropped = 0;
    portENTER_CRITICAL(&rxMux_);
    dropped = rxOverflow_;
    rxOverflow_ = 0;
    portEXIT_CRITICAL(&rxMux_);
    overflowLogMs_ = millis();
    ESP_LOGW(TAG, "RX pulse buffer overrun: %lu burst(s) longer than %u pulses discarded", (unsigned long)dropped, (unsigned)kPulseBufferSize);
  }
}
