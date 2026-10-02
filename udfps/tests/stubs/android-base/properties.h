#pragma once
#include <string>
namespace android::base {
inline std::string fingerprintTestVendor = "goodix_us";
inline std::string GetProperty(const std::string &, const std::string &) {
  return fingerprintTestVendor;
}
}
