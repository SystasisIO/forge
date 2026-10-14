#pragma once

#include "builder_state.hxx"

namespace forge::app {

struct application_builder::impl {
   builder_state state;
};

} // namespace forge::app
