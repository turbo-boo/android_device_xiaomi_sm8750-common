#pragma once
#include <unistd.h>
namespace android::base {
class unique_fd {
public:
  explicit unique_fd(int fd) : mFd(fd) {}
  ~unique_fd() { if (mFd >= 0) close(mFd); }
  int get() const { return mFd; }
private:
  int mFd;
};
}
