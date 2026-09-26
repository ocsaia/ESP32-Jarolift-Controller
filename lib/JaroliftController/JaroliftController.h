// JaroliftController.h
#ifndef JARO_LIFT_CONTROLLER_H
#define JARO_LIFT_CONTROLLER_H

#include "KeeloqLib.h"
#include "ShadeDetector.h"
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
  int16_t getRssiDbm();

  /*
   * Runtime radio diagnostics.
   *
   * Everything a received frame has to survive, counted at the point where it
   * could be lost: an edge on GDO2, a sync pulse that starts a frame, the pulses
   * that follow it, and whether the frame completed or was thrown away part way.
   * When a remote "does nothing", these say which stage it died in - no signal
   * on the pin at all, a signal that never forms a frame, or frames that form
   * and are lost before loop() decodes them.
   *
   * The ISR counts unconditionally; it is a handful of increments per edge and
   * keeping it branch-free is cheaper than asking whether anyone is listening.
   */
  struct RxDiagnostics {
    uint32_t edges;            // every GDO2 transition the ISR saw
    uint32_t syncs;            // sync pulses (low 3650..4300 us) - each opens a frame
    uint32_t partialShort;     // abandoned after 16..47 pulses
    uint32_t partialLong;      // abandoned after 48+ pulses without being a decodable frame
    uint32_t completeLost;     // a whole frame (sync + 65..75 pulses) overwritten before decoding
    uint16_t longestAbandoned; // most pulses any abandoned frame reached
    uint32_t frames;           // frames that were decoded
    uint32_t overflows;        // bursts longer than the pulse buffer
    bool irqArmed;             // is the RX interrupt attached right now - free to read, no SPI
  };

  // Read-back of the receiver as the chip reports it, not as it was configured.
  // Loop context only: every register access costs 10 ms (see wait_Miso() in
  // cc1101.cpp), so this is for on-demand use, never for a polling path.
  struct RadioStatus {
    bool initOK;
    bool irqArmed;
    int irqPin;
    int gdo2Level; // -1 if the pin is not a valid GPIO
    uint8_t marcState;
    int16_t rssiDbm;
    uint8_t iocfg2;   // expected 0x0D: GDO2 carries the asynchronous serial data
    uint8_t pktctrl0; // expected 0x32: asynchronous serial mode
    uint8_t mdmcfg2;  // modulation format in bits 6:4, 3 = ASK/OOK
    uint8_t mdmcfg4;  // channel filter bandwidth in bits 7:4
    uint32_t freqWord;
  };

  // reset == false peeks without disturbing the window a periodic reader owns
  void takeRxDiagnostics(RxDiagnostics &out, bool reset = true);
  void getRadioStatus(RadioStatus &out);

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

  // Diagnostic counters, written by the ISR under rxMux_ and taken out by
  // takeRxDiagnostics(). diagFrames_ is only touched from loop().
  volatile uint32_t diagEdges_ = 0;
  volatile uint32_t diagSyncs_ = 0;
  volatile uint32_t diagPartialShort_ = 0;
  volatile uint32_t diagPartialLong_ = 0;
  volatile uint32_t diagCompleteLost_ = 0;
  volatile uint16_t diagLongest_ = 0;
  volatile uint32_t diagOverflows_ = 0;
  uint32_t diagFrames_ = 0;
  void noteDiscardedFrame(); // ISR context, caller holds rxMux_

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
  bool rxIrqAttached_; // D3: TX detaches the RX ISR, so attach/detach must stay symmetric
  int rxIrqPin_;       // pin the ISR is attached to - a re-init may change gpio_.gdo2

  // SHADE detection (D1): a remote sends SHADE as a long press on STOP, which
  // arrives as a run of STOP frames - see ShadeDetector.h.
  ShadeDetector shade_;

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
