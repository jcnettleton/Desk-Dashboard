// Minimal Print class shim for Adafruit GFX on desktop
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>

class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) = 0;

  virtual size_t write(const uint8_t *buf, size_t size) {
    size_t n = 0;
    while (size--) { if (write(*buf++)) n++; else break; }
    return n;
  }

  size_t print(const char *s) {
    size_t n = 0;
    while (*s) { n += write((uint8_t)*s++); }
    return n;
  }

  size_t print(char c) { return write((uint8_t)c); }

  size_t print(int val) {
    char buf[16]; snprintf(buf, sizeof(buf), "%d", val);
    return print(buf);
  }

  size_t print(unsigned int val) {
    char buf[16]; snprintf(buf, sizeof(buf), "%u", val);
    return print(buf);
  }

  size_t print(long val) {
    char buf[24]; snprintf(buf, sizeof(buf), "%ld", val);
    return print(buf);
  }

  size_t print(unsigned long val) {
    char buf[24]; snprintf(buf, sizeof(buf), "%lu", val);
    return print(buf);
  }

  size_t println(const char *s = "") {
    size_t n = print(s);
    n += write((uint8_t)'\r');
    n += write((uint8_t)'\n');
    return n;
  }

  size_t println(int val) {
    size_t n = print(val);
    n += println();
    return n;
  }
};

// String class stub — only enough for GFX to compile
class String {
public:
  String() : _buf(nullptr), _len(0) {}
  String(const char *s) : _buf(strdup(s ? s : "")), _len(strlen(s ? s : "")) {}
  String(const String &o) : _buf(strdup(o._buf ? o._buf : "")), _len(o._len) {}
  ~String() { free(_buf); }
  String &operator=(const String &o) {
    if (this != &o) { free(_buf); _buf = strdup(o._buf ? o._buf : ""); _len = o._len; }
    return *this;
  }
  const char *c_str() const { return _buf ? _buf : ""; }
  unsigned int length() const { return _len; }
  char charAt(unsigned int i) const { return (i < _len && _buf) ? _buf[i] : 0; }
  char operator[](unsigned int i) const { return charAt(i); }
private:
  char *_buf;
  unsigned int _len;
};
