/* SPDX-License-Identifier: Apache-2.0 */

#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>
#include "../UdfpsWorker.h"

// Exercise the production implementation and its real patched HAL headers.
// Android Binder, properties and hardware are replaced by host-only stubs.
#define private public
#include "../UdfpsHandler.cpp"
#undef private

namespace aidl::vendor::xiaomi::hw::touchfeature {
ITouchFeature::ITouchFeature() = default;
ITouchFeature::~ITouchFeature() = default;
std::shared_ptr<ITouchFeature> ITouchFeature::fromBinder(const ndk::SpAIBinder &) {
  return nullptr;
}
}

using namespace std::chrono_literals;

struct Backend {
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<int> presses;
  bool blockPress = false;
  bool pressEntered = false;
  bool resumePress = false;
  std::function<void()> onPress;
};

static Backend *backend;

static int extCmd(fingerprint_device_t *, int32_t command, int32_t value) {
  if (command != COMMAND_FOD_PRESS_STATUS)
    return 0;
  std::function<void()> callback;
  {
    std::unique_lock<std::mutex> lock(backend->mutex);
    backend->presses.push_back(value);
    if (value == PARAM_FOD_PRESSED) {
      backend->pressEntered = true;
      callback = backend->onPress;
      backend->condition.notify_all();
      if (backend->blockPress)
        assert(backend->condition.wait_for(lock, 2s,
                                          [] { return backend->resumePress; }));
    }
  }
  if (callback)
    callback();
  return 0;
}

static void flush(SM8750UdfpsHandler &handler) {
  handler.mWorker.runSync([] {});
}

static void press(SM8750UdfpsHandler &handler, bool synchronous = true) {
  auto task = [&handler] {
    handler.mTouchX = 600;
    handler.mTouchY = 2036;
    handler.handlePressState(true);
  };
  if (synchronous)
    handler.mWorker.runSync(task);
  else
    handler.mWorker.post(task);
}

static std::vector<int> pressureCommands(Backend &state) {
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.presses;
}

static void assertIdle(SM8750UdfpsHandler &handler) {
  handler.mWorker.runSync([&handler] {
    assert(!handler.mOperationActive);
    assert(!handler.mCaptureSent);
    assert(!handler.mRetryPending);
  });
}

static void cancelDuringPress() {
  Backend state;
  backend = &state;
  state.blockPress = true;
  fingerprint_device_t device{};
  device.extCmd = extCmd;
  SM8750UdfpsHandler handler;
  handler.init(&device);
  handler.onAuthenticateStarted();
  press(handler, false);
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    assert(state.condition.wait_for(lock, 2s, [&] { return state.pressEntered; }));
  }
  auto canceled = std::async(std::launch::async, [&] { handler.cancel(); });
  assert(canceled.wait_for(10ms) == std::future_status::timeout);
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.resumePress = true;
    state.condition.notify_all();
  }
  assert(canceled.wait_for(2s) == std::future_status::ready);
  canceled.get();
  assertIdle(handler);
  assert((pressureCommands(state) == std::vector<int>{1, 0}));
  std::cout << "PASS cancel during in-flight press\n";
}

static void synchronousErrorCallback() {
  Backend state;
  backend = &state;
  fingerprint_device_t device{};
  device.extCmd = extCmd;
  SM8750UdfpsHandler handler;
  state.onPress = [&] { handler.onError(); };
  handler.init(&device);
  handler.onEnrollStarted();
  press(handler);
  flush(handler);
  assertIdle(handler);
  assert((pressureCommands(state) == std::vector<int>{1, 0}));
  std::cout << "PASS synchronous vendor error callback\n";
}

static void synchronousCancelCallback() {
  Backend state;
  backend = &state;
  fingerprint_device_t device{};
  device.extCmd = extCmd;
  SM8750UdfpsHandler handler;
  state.onPress = [&] { handler.cancel(); };
  handler.init(&device);
  handler.onAuthenticateStarted();
  press(handler);
  flush(handler);
  assertIdle(handler);
  assert((pressureCommands(state) == std::vector<int>{1, 0}));
  std::cout << "PASS reentrant cancellation\n";
}

static void pendingRetryDestruction() {
  Backend state;
  backend = &state;
  fingerprint_device_t device{};
  device.extCmd = extCmd;
  {
    SM8750UdfpsHandler handler;
    handler.init(&device);
    handler.onEnrollStarted();
    press(handler);
    handler.onAcquired(static_cast<int32_t>(AcquiredInfo::VENDOR), 41);
    flush(handler);
    handler.mWorker.runSync([&] { assert(handler.mRetryPending); });
  }
  // ASan must catch any detached retry that outlives the handler.
  std::this_thread::sleep_for(75ms);
  assert((pressureCommands(state) == std::vector<int>{1, 0}));
  std::cout << "PASS destruction with pending Goodix retry\n";
}

static void boundedRetries() {
  Backend state;
  backend = &state;
  fingerprint_device_t device{};
  device.extCmd = extCmd;
  SM8750UdfpsHandler handler;
  state.onPress = [&] {
    handler.onAcquired(static_cast<int32_t>(AcquiredInfo::VENDOR), 41);
  };
  handler.init(&device);
  handler.onEnrollStarted();
  press(handler);
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    assert(state.condition.wait_for(lock, 2s, [&] { return state.presses.size() >= 7; }));
  }
  std::this_thread::sleep_for(75ms);
  flush(handler);
  handler.mWorker.runSync([&] { assert(handler.mRetryCount == 3); });
  handler.cancel();
  assert((pressureCommands(state) == std::vector<int>{1, 0, 1, 0, 1, 0, 1, 0}));
  std::cout << "PASS Goodix retry bound\n";
}

static void retryCannotCrossOperation() {
  Backend state;
  backend = &state;
  fingerprint_device_t device{};
  device.extCmd = extCmd;
  SM8750UdfpsHandler handler;
  handler.init(&device);
  handler.onEnrollStarted();
  press(handler);
  handler.onAcquired(static_cast<int32_t>(AcquiredInfo::VENDOR), 41);
  flush(handler);
  handler.cancel();
  handler.onAuthenticateStarted();
  press(handler);
  std::this_thread::sleep_for(75ms);
  flush(handler);
  assert((pressureCommands(state) == std::vector<int>{1, 0, 1}));
  handler.cancel();
  std::cout << "PASS stale retry after operation change\n";
}

int main() {
  cancelDuringPress();
  synchronousErrorCallback();
  synchronousCancelCallback();
  pendingRetryDestruction();
  boundedRetries();
  retryCannotCrossOperation();
}
