# UDFPS host regression tests

Apply dada's fingerprint patches to crDroid `hardware/xiaomi` first, then run:

```sh
bash device/xiaomi/sm8750-common/udfps/tests/run-host-tests.sh hardware/xiaomi
```

Requires a C++17 compiler with AddressSanitizer and UndefinedBehaviorSanitizer.
The tests compile the production `UdfpsHandler.cpp` and patched legacy HAL
headers. Stubs replace Android Binder, properties and hardware; they do not
validate the stock TouchFeature ABI, kernel nodes, SELinux or real sensor/TEE
behavior. A full Android build and device tests are still required.

Cases cover cancellation during a blocked capture command, synchronous HAL
error/cancel callbacks, destruction while a Goodix retry is pending, the three
retry limit, and invalidation of retries when an operation changes.

On runners where LeakSanitizer cannot access `/proc`, run with
`ASAN_OPTIONS=detect_leaks=0`. Address and undefined behavior checks remain enabled.
