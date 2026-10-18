module;

#include <memory>
#include <utility>

module forge.swarm.core.store;

namespace forge::swarm::core {

checkpoint_store::~checkpoint_store() = default;
store::~store() = default;
store::writer::~writer() = default;
store::lease::lease() = default;
store::lease::~lease() = default;
store::lease::lease(lease&&) noexcept = default;
store::lease& store::lease::operator=(lease&&) noexcept = default;
store::lease::lease(std::shared_ptr<void> guard) : _guard{std::move(guard)} {}
void store::lease::release() noexcept { _guard.reset(); }
bool store::lease::active() const noexcept { return _guard != nullptr; }
store::lease store::make_lease(std::shared_ptr<void> guard) { return lease{std::move(guard)}; }

} // namespace forge::swarm::core
