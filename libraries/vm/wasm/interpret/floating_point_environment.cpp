module;

extern "C" {
#include <softfloat.h>
}

module forge.vm.wasm.interpret.floating_point_environment;

namespace forge::vm::wasm::interpret {

floating_point_environment::floating_point_environment() noexcept
    : _rounding(softfloat_roundingMode), _tininess(softfloat_detectTininess), _flags(softfloat_exceptionFlags),
      _precision(extF80_roundingPrecision) {
   softfloat_roundingMode = softfloat_round_near_even;
   softfloat_detectTininess = softfloat_tininess_afterRounding;
   softfloat_exceptionFlags = 0;
   extF80_roundingPrecision = 80;
}

floating_point_environment::~floating_point_environment() noexcept {
   softfloat_roundingMode = _rounding;
   softfloat_detectTininess = _tininess;
   softfloat_exceptionFlags = _flags;
   extF80_roundingPrecision = _precision;
}

} // namespace forge::vm::wasm::interpret
