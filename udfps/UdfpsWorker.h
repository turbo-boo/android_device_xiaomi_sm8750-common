/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

// Serialize touch state and HAL commands. Callbacks only enqueue work, so a
// synchronous vendor callback cannot re-enter extCmd or deadlock its caller.
class UdfpsWorker {
public:
  UdfpsWorker() : mThread([this] { run(); }) {}
  ~UdfpsWorker() { stopAndJoin(); }

  UdfpsWorker(const UdfpsWorker &) = delete;
  UdfpsWorker &operator=(const UdfpsWorker &) = delete;

  bool post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mMutex);
      if (mStopping)
        return false;
      mTasks.push_back(std::move(task));
    }
    mCondition.notify_one();
    return true;
  }

  void postDelayed(std::function<void()> task,
                   std::chrono::milliseconds delay) {
    {
      std::lock_guard<std::mutex> lock(mMutex);
      if (mStopping)
        return;
      mDelayed.emplace(std::chrono::steady_clock::now() + delay,
                       std::move(task));
    }
    mCondition.notify_one();
  }

  void runSync(std::function<void()> task) {
    // Never block or re-enter the HAL from its own callback thread.
    if (std::this_thread::get_id() == mThread.get_id()) {
      post(std::move(task));
      return;
    }
    auto done = std::make_shared<std::promise<void>>();
    auto result = done->get_future();
    if (post([task = std::move(task), done] {
          task();
          done->set_value();
        }))
      result.wait();
  }

  void stopAndJoin() {
    {
      std::lock_guard<std::mutex> lock(mMutex);
      mStopping = true;
      mDelayed.clear();
    }
    mCondition.notify_one();
    if (mThread.joinable())
      mThread.join();
  }

private:
  void run() {
    std::unique_lock<std::mutex> lock(mMutex);
    for (;;) {
      std::function<void()> task;
      // Drain immediate work first: a queued cancel invalidates pending retries.
      if (!mTasks.empty()) {
        task = std::move(mTasks.front());
        mTasks.pop_front();
      } else if (mStopping) {
        return;
      } else if (!mDelayed.empty()) {
        const auto deadline = mDelayed.begin()->first;
        if (std::chrono::steady_clock::now() < deadline) {
          mCondition.wait_until(lock, deadline);
          continue;
        }
        task = std::move(mDelayed.begin()->second);
        mDelayed.erase(mDelayed.begin());
      } else {
        mCondition.wait(lock);
        continue;
      }
      lock.unlock();
      task();
      lock.lock();
    }
  }

  std::mutex mMutex;
  std::condition_variable mCondition;
  std::deque<std::function<void()>> mTasks;
  std::multimap<std::chrono::steady_clock::time_point, std::function<void()>>
      mDelayed;
  bool mStopping = false;
  std::thread mThread;
};
