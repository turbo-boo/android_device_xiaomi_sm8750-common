/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-FileCopyrightText: Paranoid Android
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler.xiaomi_sm8750"

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <android/binder_manager.h>

#include <atomic>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/input.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

#include "UdfpsHandler.h"
#include "TouchFeatureCompat.h"
#include "UdfpsWorker.h"

#define COMMAND_FOD_PRESS_STATUS 1
#define COMMAND_FOD_PRESS_X 2
#define COMMAND_FOD_PRESS_Y 3
#define COMMAND_DISPLAY_STATUS 14
#define COMMAND_FILM_MODE 15
#define PARAM_FOD_PRESSED 1
#define PARAM_FOD_RELEASED 0

#define TOUCH_FEATURE_SERVICE                                                  \
  "vendor.xiaomi.hw.touchfeature.ITouchFeature/default"
#define TOUCH_ID_PRIMARY 0
#define TOUCH_MODE_FOD_ENABLE 10
#define TOUCH_MODE_FOD_AUTH 41
#define TOUCH_MODE_FILM 1015

#define FOD_PRESS_STATUS_PATH "/sys/class/touch/touch_dev/fod_press_status"
#define TOUCH_RESOLUTION_FACTOR_PATH                                           \
  "/sys/class/touch/touch_dev/resolution_factor"
#define TOUCH_INPUT_NAME "Xiaomi_Touch_Input_0"
#define QCOM_US_INTR2_PATH "/sys/class/xiaomi_fp/xiaomi-fp/device/intr2"
#define DISPLAY_PRE_POWER_PATH                                                 \
  "/sys/bus/platform/devices/soc:xiaomi_fingerprint/fingerdown"

#define DISPLAY_STATUS_ON 2
#define FILM_MODE_OFF 0

#define GOODIX_CRC_VENDOR_CODE 41
#define GOODIX_MAX_RETRIES 3

using ::aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;

namespace {

using ::aidl::vendor::xiaomi::hw::touchfeature::ITouchFeature;

static std::shared_ptr<ITouchFeature> getTouchFeature() {
  ndk::SpAIBinder binder(AServiceManager_checkService(TOUCH_FEATURE_SERVICE));
  if (binder.get() == nullptr)
    return nullptr;
  return ITouchFeature::fromBinder(binder);
}

static bool setTouchMode(int32_t mode, int32_t value) {
  auto service = getTouchFeature();
  if (service == nullptr) {
    LOG(ERROR) << "TouchFeature service is unavailable";
    return false;
  }

  int32_t result = -1;
  auto status =
      service->setModeValue(TOUCH_ID_PRIMARY, mode, value, &result);
  if (!status.isOk() || result != 0) {
    LOG(ERROR) << "Failed to set touch mode " << mode << " to " << value
               << ": status=" << status.getDescription()
               << " result=" << result;
    return false;
  }

  LOG(INFO) << "Set touch mode " << mode << " to " << value;
  return true;
}

static int32_t getTouchFilmMode() {
  auto service = getTouchFeature();
  if (service == nullptr) {
    return FILM_MODE_OFF;
  }

  std::string value;
  auto status = service->getModeCurValueString(TOUCH_ID_PRIMARY,
                                                TOUCH_MODE_FILM, &value);
  if (!status.isOk()) {
    LOG(WARNING) << "Failed to read touch film mode: "
                 << status.getDescription();
    return FILM_MODE_OFF;
  }
  if (value == "1")
    return 1;
  if (value == "2")
    return 2;
  return FILM_MODE_OFF;
}

static int openTouchInput() {
  DIR *inputDir = opendir("/dev/input");
  if (inputDir == nullptr) {
    PLOG(ERROR) << "Failed to open /dev/input";
    return -1;
  }

  int inputFd = -1;
  while (dirent *entry = readdir(inputDir)) {
    if (strncmp(entry->d_name, "event", 5) != 0)
      continue;

    std::string path = std::string("/dev/input/") + entry->d_name;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
      continue;

    char name[128] = {};
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
        name == std::string(TOUCH_INPUT_NAME)) {
      LOG(INFO) << "Monitoring FOD coordinates from " << path;
      inputFd = fd;
      break;
    }
    close(fd);
  }
  closedir(inputDir);
  return inputFd;
}

static bool writeState(const char *path, bool enabled) {
  android::base::unique_fd fd(open(path, O_WRONLY | O_CLOEXEC));
  if (fd.get() < 0) {
    PLOG(ERROR) << "Failed to open " << path;
    return false;
  }

  const char value[] = {enabled ? '1' : '0', '\n'};
  if (TEMP_FAILURE_RETRY(write(fd.get(), value, sizeof(value))) !=
      sizeof(value)) {
    PLOG(ERROR) << "Failed to write " << path;
    return false;
  }
  return true;
}

} // anonymous namespace

class SM8750UdfpsHandler : public UdfpsHandler {
public:
  ~SM8750UdfpsHandler() override {
    mStop = true;
    if (mStopPipe[1] >= 0) {
      char value = 1;
      TEMP_FAILURE_RETRY(write(mStopPipe[1], &value, sizeof(value)));
    }
    if (mMonitorThread.joinable())
      mMonitorThread.join();
    // Finish in-flight commands before the handler or its HAL device is freed.
    // Delayed retries are owned by the worker and discarded by stopAndJoin().
    mWorker.runSync([this] {
      stopOperation();
      handlePressState(false);
    });
    mWorker.stopAndJoin();
    if (mStopPipe[0] >= 0)
      close(mStopPipe[0]);
    if (mStopPipe[1] >= 0)
      close(mStopPipe[1]);
  }

  void init(fingerprint_device_t *device) {
    mDevice = device;
    const std::string fingerprintVendor =
        android::base::GetProperty("persist.vendor.sys.fp.vendor", "");
    mUseQcomIntr2 = fingerprintVendor == "qcom_us";
    mUseGoodixTouchAuth = fingerprintVendor == "goodix_us";
    LOG(INFO) << "Using QCOM intr2 handoff: " << mUseQcomIntr2;
    LOG(INFO) << "Using Goodix touch auth lifecycle: "
              << mUseGoodixTouchAuth;

    std::ifstream factorFile(TOUCH_RESOLUTION_FACTOR_PATH);
    factorFile >> mResolutionFactor;
    if (mResolutionFactor <= 0)
      mResolutionFactor = 1;

    if (pipe2(mStopPipe, O_CLOEXEC | O_NONBLOCK) != 0) {
      PLOG(ERROR) << "Failed to create touch monitor stop pipe";
      return;
    }
    mMonitorThread = std::thread(&SM8750UdfpsHandler::monitorTouch, this);
  }

  void onFingerDown(uint32_t x, uint32_t y, float /*minor*/, float /*major*/) {
    LOG(DEBUG) << __func__ << "x: " << x << ", y: " << y;
    // Stock mfp-daemon does not trigger capture from the framework overlay.
    // Keep these coordinates only as a fallback if the touch input node is
    // absent.
    enqueue([this, x, y] {
      mFrameworkX = x;
      mFrameworkY = y;
      if (mInputUnavailable && mPhysicalPressed)
        maybeSendPress();
    });
  }

  void onFingerUp() { LOG(DEBUG) << __func__; }

  void onUiReady() { LOG(DEBUG) << __func__; }

  void onEnrollStarted() {
    LOG(INFO) << __func__;
    mWorker.runSync([this] { startOperation(); });
  }

  void onAuthenticateStarted() {
    LOG(INFO) << __func__;
    mWorker.runSync([this] { startOperation(); });
  }

  void onAcquired(int32_t result, int32_t vendorCode) {
    LOG(DEBUG) << __func__ << " result: " << result
               << " vendorCode: " << vendorCode;
    enqueue([this, result, vendorCode] { handleAcquired(result, vendorCode); });
  }

  void onEnrollmentProgress(uint32_t remaining) {
    enqueue([this, remaining] {
      if (remaining == 0) {
        stopOperation();
      } else if (mOperationActive) {
        setTouchMode(TOUCH_MODE_FOD_AUTH, 1);
      }
    });
  }

  void onError() { enqueue([this] { stopOperation(); }); }

  bool shouldForwardAcquired(int32_t result, int32_t vendorCode) {
    // QFP uses vendor acquired 28 to report an unusable ultrasonic sample.
    return static_cast<AcquiredInfo>(result) == AcquiredInfo::VENDOR &&
           vendorCode == 28;
  }

  void cancel() {
    LOG(INFO) << __func__;
    // Session::cancel calls the vendor cancel next. Release must finish first.
    mWorker.runSync([this] { stopOperation(); });
  }

  void onAuthenticationSucceeded() {
    enqueue([this] { stopOperation(); });
  }

  void onAuthenticationFailed() {
    enqueue([this] {
      if (mOperationActive)
        setTouchMode(TOUCH_MODE_FOD_AUTH, 1);
    });
  }

private:
  void enqueue(std::function<void()> task) {
    if (!mStop)
      mWorker.post(std::move(task));
  }

  void handleAcquired(int32_t result, int32_t vendorCode) {
    // Goodix reports 201 before CRC code 41; consuming it would block retry.
    if (!mUseGoodixTouchAuth &&
        static_cast<AcquiredInfo>(result) == AcquiredInfo::VENDOR &&
        (vendorCode == 201 || vendorCode == 202)) {
      // QFP has rejected this sample, but the physical contact is still down.
      // Do not synthesize pointer-up here: QFP uses the up transition to take
      // an air reference, which must happen only after the finger is gone.
      mContactConsumed = true;
    }
    if (mUseGoodixTouchAuth &&
        static_cast<AcquiredInfo>(result) == AcquiredInfo::VENDOR &&
        vendorCode == GOODIX_CRC_VENDOR_CODE) {
      // Observed on goodix_us: each tap ends ~100ms later in
      // GF_ERROR_SPI_RAW_DATA_CRC_FAILED while the finger is still down, and
      // Session swallows the vendor code so the UI stays inert. The user
      // perceives this as "tap repeatedly until it reacts". Emulate the next
      // tap in software while the contact lasts. Bounded so a persistently
      // failing sensor cannot spin on one press.
      if (mOperationActive && mPhysicalPressed && !mContactConsumed &&
          mCaptureSent && mRetryCount < GOODIX_MAX_RETRIES &&
          !mRetryPending) {
        mRetryPending = true;
        const int32_t attempt = ++mRetryCount;
        const uint64_t generation = mPressGeneration;
        const uint64_t operation = mOperationGeneration;
        LOG(INFO) << "Scheduling Goodix FOD retry, attempt " << attempt;
        // Session::notify runs on the HAL callback thread; re-entering the
        // HAL with extCmd here risks deadlock, and the sensor only reports
        // its reset IRQ ~25ms after the CRC failure. Retry asynchronously
        // after the reset so the next capture starts from a clean state.
        // The generation guard drops the retry if the finger was lifted
        // and put down again while the worker slept.
        mWorker.postDelayed([this, attempt, generation, operation] {
          // An old retry must not clear a newer contact's pending flag.
          if (generation != mPressGeneration ||
              operation != mOperationGeneration)
            return;
          mRetryPending = false;
          if (mStop || !mOperationActive || !mPhysicalPressed || mContactConsumed ||
              !mCaptureSent)
            return;
          LOG(INFO) << "Retrying Goodix FOD capture, attempt " << attempt;
          sendRelease();
          maybeSendPress();
        }, std::chrono::milliseconds(50));
      }
    }
  }

  void startOperation() {
    if (mStop || mDevice == nullptr)
      return;
    sendRelease();
    ++mOperationGeneration;
    mRetryPending = false;
    mTouchX = 0;
    mTouchY = 0;
    mCaptureSent = false;
    mContactConsumed = false;
    mRetryCount = 0;
    mPhysicalPressed = false;
    mOperationActive = true;
    mDevice->extCmd(mDevice, COMMAND_DISPLAY_STATUS, DISPLAY_STATUS_ON);
    setTouchMode(TOUCH_MODE_FOD_ENABLE, 1);
    setTouchMode(TOUCH_MODE_FOD_AUTH, 1);
  }

  void stopOperation() {
    ++mOperationGeneration;
    mRetryPending = false;
    if (!mOperationActive)
      return;
    mOperationActive = false;
    sendRelease();
    setTouchMode(TOUCH_MODE_FOD_AUTH, 0);
    setTouchMode(TOUCH_MODE_FOD_ENABLE, 0);
  }

  void sendRelease() {
    if (mCaptureSent) {
      mCaptureSent = false;
      mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_X, 0);
      mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_Y, 0);
      mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_RELEASED);
      LOG(INFO) << "Forwarded physical FOD release";
    }
  }

  void maybeSendPress() {
    if (!mOperationActive || !mPhysicalPressed || mCaptureSent ||
        mContactConsumed)
      return;

    int32_t x = mTouchX;
    int32_t y = mTouchY;
    if ((x <= 0 || y <= 0) && mInputUnavailable) {
      x = mFrameworkX;
      y = mFrameworkY;
    }
    if (x <= 0 || y <= 0)
      return;

    mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_X, x);
    mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_Y, y);
    mDevice->extCmd(mDevice, COMMAND_FILM_MODE, getTouchFilmMode());
    mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_PRESSED);
    mCaptureSent = true;
    LOG(INFO) << "Forwarded physical FOD press at " << x << "," << y;
  }

  void handlePressState(bool pressed) {
    if (pressed == mPhysicalPressed)
      return;
    mPhysicalPressed = pressed;

    if (pressed) {
      mPressGeneration++;
      // Stock mfp-daemon drives intr2 only for the qcom_us backend. Driving
      // that GPIO while goodix_us owns the sensor disrupts its SPI capture.
      if (mUseQcomIntr2)
        writeState(QCOM_US_INTR2_PATH, true);
      writeState(DISPLAY_PRE_POWER_PATH, true);
      // Stock mfp-daemon keeps Goodix FOD auth state asserted while a
      // physical contact is being captured and clears it only when the
      // operation finishes. Deasserting it here makes the touch controller
      // withdraw the sample while Goodix is still reading it, which surfaces
      // as RAW_DATA_CRC_FAILED and SPI_COMMUNICATION errors. Keep the existing
      // qcom_us handoff unchanged.
      if (mOperationActive && !mUseGoodixTouchAuth)
        setTouchMode(TOUCH_MODE_FOD_AUTH, 0);
      // Stock posts its QFP down notification asynchronously after changing
      // the touch-panel auth state. Our direct extCmd path is synchronous, so
      // arm the panel before starting capture.
      maybeSendPress();
    } else {
      if (mUseQcomIntr2)
        writeState(QCOM_US_INTR2_PATH, false);
      writeState(DISPLAY_PRE_POWER_PATH, false);
      sendRelease();
      mTouchX = 0;
      mTouchY = 0;
      mContactConsumed = false;
      mRetryCount = 0;
      mRetryPending = false;
      if (mOperationActive)
        setTouchMode(TOUCH_MODE_FOD_AUTH, 1);
    }
  }

  void handleInputEvents(int inputFd) {
    input_event events[16];
    ssize_t bytes;
    int32_t x = 0;
    int32_t y = 0;
    while ((bytes = TEMP_FAILURE_RETRY(read(inputFd, events, sizeof(events)))) >
           0) {
      size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
      for (size_t i = 0; i < count; i++) {
        const auto &event = events[i];
        if (event.type != EV_ABS || event.value <= 0)
          continue;
        if (event.code == ABS_MT_POSITION_X) {
          x = event.value / mResolutionFactor;
        } else if (event.code == ABS_MT_POSITION_Y) {
          y = event.value / mResolutionFactor;
        }
      }
    }
    enqueue([this, x, y] {
      if (x > 0)
        mTouchX = x;
      if (y > 0)
        mTouchY = y;
      maybeSendPress();
    });
  }

  void monitorTouch() {
    int statusFd =
        open(FOD_PRESS_STATUS_PATH, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (statusFd < 0) {
      PLOG(ERROR) << "Failed to open " << FOD_PRESS_STATUS_PATH;
      return;
    }

    int inputFd = openTouchInput();
    const bool inputUnavailable = inputFd < 0;
    enqueue([this, inputUnavailable] { mInputUnavailable = inputUnavailable; });
    if (inputUnavailable)
      LOG(WARNING) << "Touch input unavailable; using overlay coordinates";

    char initialState = '0';
    TEMP_FAILURE_RETRY(pread(statusFd, &initialState, sizeof(initialState), 0));

    pollfd fds[] = {
        {.fd = mStopPipe[0], .events = POLLIN, .revents = 0},
        {.fd = statusFd, .events = POLLERR | POLLPRI, .revents = 0},
        {.fd = inputFd, .events = POLLIN, .revents = 0},
    };

    while (!mStop) {
      int count = TEMP_FAILURE_RETRY(poll(fds, 3, -1));
      if (count <= 0)
        continue;
      if (fds[0].revents & POLLIN) {
        char value;
        TEMP_FAILURE_RETRY(read(mStopPipe[0], &value, sizeof(value)));
        break;
      }

      if (inputFd >= 0 && (fds[2].revents & POLLIN))
        handleInputEvents(inputFd);
      if (fds[1].revents & (POLLERR | POLLPRI)) {
        char state = '0';
        if (TEMP_FAILURE_RETRY(pread(statusFd, &state, sizeof(state), 0)) ==
            1) {
          enqueue([this, pressed = state == '1'] { handlePressState(pressed); });
        }
      }
    }

    if (inputFd >= 0)
      close(inputFd);
    close(statusFd);
  }

  fingerprint_device_t *mDevice = nullptr;
  std::thread mMonitorThread;
  int mStopPipe[2] = {-1, -1};
  int32_t mResolutionFactor = 1;
  bool mUseQcomIntr2 = false;
  bool mUseGoodixTouchAuth = false;
  std::atomic_bool mStop = false;
  // Only the worker accesses these fields and sends touch/HAL commands.
  bool mOperationActive = false;
  bool mPhysicalPressed = false;
  bool mCaptureSent = false;
  bool mContactConsumed = false;
  int32_t mRetryCount = 0;
  bool mRetryPending = false;
  uint64_t mPressGeneration = 0;
  uint64_t mOperationGeneration = 0;
  bool mInputUnavailable = true;
  int32_t mTouchX = 0;
  int32_t mTouchY = 0;
  int32_t mFrameworkX = 0;
  int32_t mFrameworkY = 0;
  UdfpsWorker mWorker;
};

static UdfpsHandler *create() { return new SM8750UdfpsHandler(); }

static void destroy(UdfpsHandler *handler) { delete handler; }

extern "C" {
UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
    .create = create,
    .destroy = destroy,
};
}
