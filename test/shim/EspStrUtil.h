#pragma once

/*
 * Stands in for the EspStrUtil library during native tests.
 *
 * The real header cannot be compiled on the host at all: it declares both
 * intToString(int) and intToString(int32_t), which are two functions on xtensa,
 * where int32_t is long, and the same function on x86, where it is int. That is
 * a property of the library, not something these tests can route around.
 *
 * It costs little. What config.cpp is being tested for is whether its two
 * halves agree - roughly two hundred fields written by configSaveToFile() and
 * read back by configLoadFromFile(), where a key written under one name and
 * read under another compiles perfectly and silently loses a value. None of
 * that weight rests on this file.
 *
 * readJSONstring is the one function here whose exact behaviour a test does
 * depend on, so it is a faithful copy of the original rather than an
 * approximation - see EspStrUtil.h in the library, readJSONstring(). Its
 * important property is that a NULL src leaves the destination untouched, which
 * is what makes a key missing from an older config file keep whatever the
 * struct already held instead of becoming an empty string.
 *
 * The password pair is deliberately NOT cryptography: a reversible transform is
 * all a round-trip test can use, and real AES here would only add a system
 * dependency to a test that is not about the cipher. It is obviously fake so
 * that nobody mistakes it for the real thing.
 */

#include <cstddef>
#include <cstdio>
#include <cstring>

class EspStrUtil {
public:
  static inline void readJSONstring(char *dest, size_t size, const char *src) {
    const char *check = src;
    if (check != NULL) {
      snprintf(dest, size, "%s", src);
    }
  }

  // djb2, same as the library's
  static inline unsigned long hash(void *str, size_t len) {
    unsigned char *p = (unsigned char *)str;
    unsigned long h = 5381;
    for (size_t i = 0; i < len; i++) {
      h = ((h << 5) + h) + p[i];
    }
    return h;
  }

  // A visible, reversible stand-in: byte-rotate, then hex. Not encryption.
  static inline bool encryptPassword(const char *input, const unsigned char *key, char *output, size_t maxOutputSize) {
    if (input == NULL || key == NULL || output == NULL || maxOutputSize == 0) {
      return false;
    }
    size_t len = strlen(input);
    if (len > 128 || (len * 2 + 1) > maxOutputSize) {
      return false;
    }
    for (size_t i = 0; i < len; i++) {
      unsigned char c = (unsigned char)(input[i] ^ key[i % 16]);
      snprintf(output + i * 2, 3, "%02x", c);
    }
    output[len * 2] = 0;
    return true;
  }

  static inline bool decryptPassword(const char *input, const unsigned char *key, char *output, size_t maxOutputSize) {
    if (input == NULL || key == NULL || output == NULL || maxOutputSize == 0) {
      return false;
    }
    size_t len = strlen(input);
    if ((len % 2) != 0 || (len / 2 + 1) > maxOutputSize) {
      return false;
    }
    for (size_t i = 0; i < len; i += 2) {
      unsigned int byte = 0;
      if (sscanf(input + i, "%2x", &byte) != 1) {
        return false;
      }
      output[i / 2] = (char)((unsigned char)byte ^ key[(i / 2) % 16]);
    }
    output[len / 2] = 0;
    return true;
  }
};
