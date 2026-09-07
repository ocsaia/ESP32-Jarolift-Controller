#pragma once

/*
 * In-memory stand-in for the LittleFS partition.
 *
 * config.cpp is worth testing precisely because it crosses this boundary: it
 * serialises a two-hundred-field struct out to a file and parses it back, and
 * a key written under one name and read under another compiles perfectly. To
 * exercise that on the host the file only has to be a buffer, which is all this
 * is - plus enough of the Arduino File surface for ArduinoJson to stream
 * through it.
 *
 * Tests reach the contents through fakeFsRead()/fakeFsWrite(), so a test can
 * plant an old config file and then look at what the firmware wrote back.
 */

#include <cstdint>
#include <cstring>
#include <map>
#include <string>

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

inline std::map<std::string, std::string> &fakeFsFiles() {
  static std::map<std::string, std::string> files;
  return files;
}

class FakeFile {
public:
  FakeFile() : name_(), pos_(0), writable_(false), valid_(false) {}
  FakeFile(const std::string &name, bool writable, bool valid) : name_(name), pos_(0), writable_(writable), valid_(valid) {}

  explicit operator bool() const { return valid_; }
  bool operator!() const { return !valid_; }

  // --- reader side, what deserializeJson() uses ---
  int available() {
    const std::string &d = data();
    return (int)(pos_ < d.size() ? d.size() - pos_ : 0);
  }
  int read() {
    const std::string &d = data();
    return pos_ < d.size() ? (unsigned char)d[pos_++] : -1;
  }
  int peek() {
    const std::string &d = data();
    return pos_ < d.size() ? (unsigned char)d[pos_] : -1;
  }
  size_t readBytes(char *buffer, size_t length) {
    const std::string &d = data();
    size_t n = 0;
    while (n < length && pos_ < d.size()) {
      buffer[n++] = d[pos_++];
    }
    return n;
  }

  // --- writer side, what serializeJson() uses ---
  size_t write(uint8_t c) {
    if (!writable_) {
      return 0;
    }
    fakeFsFiles()[name_].push_back((char)c);
    return 1;
  }
  size_t write(const uint8_t *buffer, size_t size) {
    if (!writable_) {
      return 0;
    }
    fakeFsFiles()[name_].append((const char *)buffer, size);
    return size;
  }

  void close() {}
  size_t size() { return data().size(); }

private:
  const std::string &data() {
    static const std::string empty;
    auto it = fakeFsFiles().find(name_);
    return it == fakeFsFiles().end() ? empty : it->second;
  }
  std::string name_;
  size_t pos_;
  bool writable_;
  bool valid_;
};

typedef FakeFile File;

class FakeLittleFS {
public:
  bool begin(bool formatOnFail = false) {
    (void)formatOnFail;
    return true;
  }
  File open(const char *path, const char *mode = FILE_READ) {
    std::string name(path);
    if (mode != NULL && mode[0] == 'w') {
      fakeFsFiles()[name] = std::string();
      return File(name, true, true);
    }
    // Opening a file that is not there yields a falsy File, the way the real
    // one does - the "no config yet, go to setup mode" path depends on it.
    if (fakeFsFiles().find(name) == fakeFsFiles().end()) {
      return File(name, false, false);
    }
    return File(name, false, true);
  }
  bool remove(const char *path) { return fakeFsFiles().erase(std::string(path)) > 0; }
  bool exists(const char *path) { return fakeFsFiles().count(std::string(path)) > 0; }
};

extern FakeLittleFS LittleFS;

// --- helpers for the tests themselves ---
inline void fakeFsWrite(const char *path, const std::string &content) { fakeFsFiles()[std::string(path)] = content; }
inline std::string fakeFsRead(const char *path) {
  auto it = fakeFsFiles().find(std::string(path));
  return it == fakeFsFiles().end() ? std::string() : it->second;
}
inline void fakeFsClear() { fakeFsFiles().clear(); }
