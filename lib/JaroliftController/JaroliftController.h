// JaroliftController.h
#ifndef JARO_LIFT_CONTROLLER_H
#define JARO_LIFT_CONTROLLER_H

#include "KeeloqLib.h"
#include "cc1101.h"
#include <Arduino.h>
#include <EEPROM.h>
#include <nvs.h>
#include <nvs_flash.h>

class JaroliftController {
public:
  // Kapselung der GPIO- und Konfigurationsdaten
  struct GPIO {
    int sck;
    int miso;
    int mosi;
    int cs;
    int gdo0; // TX
    int gdo2; // RX
  };

  struct Config {
    uint32_t serial;
    bool learnMode;
    unsigned long masterMSB;
    unsigned long masterLSB;
  };

  // enum of shutter commands
  enum commands {
    CMD_UP,
    CMD_DOWN,
    CMD_STOP,
    CMD_SHADE,
    CMD_SET_SHADE,
  };

  JaroliftController();
  ~JaroliftController();

  // Lebenszyklus
  void begin();
  void loop();

  // Einzelbefehle
  void cmdChannel(commands cmd, uint8_t channel);

  // Gruppenbefehle
  void cmdGroup(commands cmd, uint16_t groupMask);

  // Service Commands
  void cmdLearn(uint8_t channel);
  void cmdUnlearn(uint8_t channel);
  void cmdSetEndPointUp(uint8_t channel);
  void cmdDeleteEndPointUp(uint8_t channel);
  void cmdSetEndPointDown(uint8_t channel);
  void cmdDeleteEndPointDown(uint8_t channel);

  // Konfigurationssetter
  void setGPIO(int sck, int miso, int mosi, int cs, int gdo0, int gdo2);
  void setBaseSerial(uint32_t serial);
  void setLegacyLearnMode(bool legacyMode);
  void setKeys(unsigned long masterMSB, unsigned long masterLSB);
  void setRemoteCallback(void (*callback)(uint32_t serial, int8_t function, uint16_t channel)) { remoteCallback = callback; }

  /*
   * Hand the library a way to feed the caller's watchdog.
   *
   * The service sequences transmit for seconds at a time and run entirely
   * inside the caller's task, so nothing else in that task gets to run - see
   * radioTx() for where this is called and why that point is safe. Optional:
   * with no callback set the library behaves exactly as before.
   */
  void setWatchdogCallback(void (*callback)()) { watchdogCallback = callback; }

  // Hilfsfunktionen
  uint16_t getDeviceCounter();
  void setDeviceCounter(uint16_t newDevCnt);
  uint32_t getSerial(uint8_t channel);
  bool getCC1101State();
  uint8_t getRssi();

private:
  // Instanzvariablen (anstatt globaler Variablen)
  GPIO gpio_;
  Config config_;
  uint16_t devCount_;

  // D4: the NVS handle is opened once and kept, and the counter is mirrored in
  // RAM. It used to be opened and closed on every single access, plus a
  // delay(100) per write - about 800 ms of pure waiting inside cmdUnlearn().
  nvs_handle_t nvsHandle_;
  bool nvsOpen_;
  bool devCountValid_;

  // 32 bit KeeLoq device keys. Keeloq::decrypt() produces them as unsigned long
  // and Keeloq() consumes them as unsigned long again; holding them in a signed
  // int made every key with bit 31 set depend on implementation-defined
  // conversion behaviour for no reason.
  uint32_t deviceKeyMSB_;
  uint32_t deviceKeyLSB_;

  uint64_t button_;
  uint8_t discL_;
  uint8_t discH_;
  uint16_t disc_;

  uint64_t newSerial_;

  uint32_t encrypted_;
  uint64_t pack_;

  // Empfangspuffer
  static constexpr size_t kPulseBufferSize = 216;
  // Every value that is stored here is range-checked to less than 4300 µs first,
  // so uint16_t holds all of them. Halving the two buffers pays for the frame
  // snapshot below twice over and halves the time the copy masks interrupts.
  volatile uint16_t lowBuf_[kPulseBufferSize];
  volatile uint16_t hiBuf_[kPulseBufferSize];
  volatile unsigned int pbWrite_;
  volatile uint32_t rxOverflow_; // A1: bursts dropped because they filled the buffer

  // Spinlock protecting lowBuf_/hiBuf_/pbWrite_/rxOverflow_ against the RX ISR.
  // The ISR can run on the other core, so masking interrupts alone would not do.
  portMUX_TYPE rxMux_ = portMUX_INITIALIZER_UNLOCKED;

  // Snapshot of one frame, taken under rxMux_ so decoding never races the ISR.
  // 76 covers the longest accepted frame (pbWrite_ <= 75); decoding reads up to
  // index 72. Kept as members, not locals, to leave the loop() stack untouched.
  static constexpr size_t kFrameSnapshotSize = 76;
  uint16_t snapLow_[kFrameSnapshotSize];
  uint16_t snapHi_[kFrameSnapshotSize];

  // Empfangsdaten
  uint32_t rxSerial_;
  uint32_t rxHopCode_;
  uint8_t rxFunction_;
  uint16_t rxDiscH_;

  // Laufzeitzustand
  bool initOK_;
  bool rxDataReady_;
  bool rxIrqAttached_; // D3: TX detaches the RX ISR, so attach/detach must stay symmetric
  int rxIrqPin_;       // pin the ISR is attached to - a re-init may change gpio_.gdo2

  // SHADE detection (D1): a remote sends SHADE as a long press on STOP, which
  // arrives as a run of STOP frames from one remote for one set of channels.
  uint32_t stopRunSerial_;
  uint16_t stopRunChannel_;
  uint8_t stopRunCount_;
  bool stopRunReported_;
  unsigned long stopRunLastMs_;

  unsigned long overflowLogMs_; // rate limit for the A1 overrun warning

  // Hardware-Modul
  CC1101 cc1101_;

  // Konstanten
  static constexpr int kLowPulse = 400; // in µs
  static constexpr int kHighPulse = 800;
  static constexpr int kDebounce = 200;
  static constexpr uint8_t kSyncWord = 199;

  static constexpr uint8_t FCT_CODE_UP = 0x8;
  static constexpr uint8_t FCT_CODE_DOWN = 0x2;
  static constexpr uint8_t FCT_CODE_STOP = 0x4;
  static constexpr uint8_t FCT_CODE_UPDOWN = 0xA;
  static constexpr uint8_t FCT_CODE_SHADE = 0x3; // never sent over the air - derived from a long STOP press

  // B3: discLowArr_/discHighArr_ have one entry per channel, and every public
  // command takes a uint8_t channel that ends up as an index into them.
  static constexpr uint8_t kMaxChannels = 16;

  // D1: consecutive STOP frames that count as a long press. The original fired at
  // "steadyCount_ > 10" and loop() needs ~250 ms to recover per decoded frame, so
  // this is roughly three seconds of holding the button.
  static constexpr uint8_t kShadeStopFrames = 11;
  // A pause longer than this ends the run: two separate STOP presses must not add up.
  static constexpr unsigned long kShadeRunGapMs = 1500;

  static constexpr char *TAG = "JARO-LIB"; // LOG TAG

  // Diskriminierungsarrays (analog zu den Originalwerten)
  uint8_t discLowArr_[16];
  uint8_t discHighArr_[16];

  // Hilfsfunktionen
  void updateDeviceCounter(bool increment);
  bool openNvs();
  bool channelValid(uint8_t channel, const char *cmdName) const;

  void radioTxFrame(int length);
  void radioTxGroupH();
  void radioTx(int repetitions);
  void attachRxInterrupt();
  void detachRxInterrupt();
  void enterRx();
  void enterTx();
  void processRxData();
  void generateKey();       // Schlüsselgenerierung (keygen)
  void generateEncrypted(); // Verschlüsseln (keeloq)

  void rxKeyGen();
  uint32_t rxDecode();

  void (*remoteCallback)(uint32_t serial, int8_t function, uint16_t channel) = nullptr;
  void (*watchdogCallback)() = nullptr;

  // Interrupt-Service-Routine (ISR) für RX-Messung
  static void IRAM_ATTR radioRxMeasureISR();
  void handleRadioRxMeasure();

  // Singleton-Zeiger für den ISR-Zugriff
  static JaroliftController *instance_;
};

#endif // JARO_LIFT_CONTROLLER_H
