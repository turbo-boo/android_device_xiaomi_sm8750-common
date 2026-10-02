#pragma once
struct TestLog {
  template <class T> TestLog &operator<<(const T &) { return *this; }
};
#define LOG(level) TestLog{}
#define PLOG(level) TestLog{}
