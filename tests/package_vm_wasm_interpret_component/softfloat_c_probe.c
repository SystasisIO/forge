// The installed owner's PUBLIC definition must reach C as well as C++.
#ifndef THREAD_LOCAL
#error "The installed SoftFloat owner did not propagate THREAD_LOCAL to C"
#endif

#include <softfloat.h>

void forge_package_softfloat_c_addresses(const void** addresses) {
   addresses[0] = &softfloat_roundingMode;
   addresses[1] = &softfloat_detectTininess;
   addresses[2] = &softfloat_exceptionFlags;
   addresses[3] = &extF80_roundingPrecision;
}

int forge_package_softfloat_c_defaults(void) {
   return softfloat_roundingMode == softfloat_round_near_even &&
          softfloat_detectTininess == softfloat_tininess_afterRounding && softfloat_exceptionFlags == 0 &&
          extF80_roundingPrecision == 80;
}

void forge_package_softfloat_c_set(uint_fast8_t rounding, uint_fast8_t tininess, uint_fast8_t flags,
                                   uint_fast8_t precision) {
   softfloat_roundingMode = rounding;
   softfloat_detectTininess = tininess;
   softfloat_exceptionFlags = flags;
   extF80_roundingPrecision = precision;
}

uint32_t forge_package_softfloat_c_add(uint32_t left, uint32_t right) {
   return f32_add((float32_t){left}, (float32_t){right}).v;
}
