#pragma once
#include "binder_manager.h"
#include <string>
namespace ndk {
class SpAIBinder {
public:
  explicit SpAIBinder(AIBinder *binder) : mBinder(binder) {}
  AIBinder *get() const { return mBinder; }
private:
  AIBinder *mBinder;
};
class ScopedAStatus {
public:
  bool isOk() const { return true; }
  std::string getDescription() const { return "OK"; }
};
class ICInterface {
public:
  virtual ~ICInterface() = default;
};
}
