module;

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <forge/log/macros.hpp>

module forge.net.p2p.resource_manager;

import forge.log.logger;

#include "details/resource_manager_state.hxx"

namespace forge::net::p2p {

static_assert(std::is_nothrow_move_constructible_v<peer_id>);
static_assert(std::is_nothrow_move_constructible_v<std::optional<peer_id>>);

resource_manager::resource_manager() : resource_manager(limits{}) {}

resource_manager::resource_manager(limits value) : state_(std::make_shared<state>(std::move(value))) {}

resource_manager::~resource_manager() = default;

const resource_manager::limits& resource_manager::configured_limits() const noexcept {
   static const auto defaults = limits{};
   return state_ ? state_->configured_limits() : defaults;
}

resource_manager::snapshot resource_manager::current() const noexcept {
   return state_ ? state_->current() : snapshot{};
}

resource_manager::reservation_result<resource_manager::lifecycle_reservation>
resource_manager::reserve_lifecycle() noexcept {
   if (!state_) {
      return reservation_result<lifecycle_reservation>{transition_result::invalid_transition};
   }
   auto attempt = state_->reserve_lifecycle();
   if (!attempt.reservation) {
      return reservation_result<lifecycle_reservation>{attempt.outcome};
   }
   return reservation_result<lifecycle_reservation>{lifecycle_reservation{state_, std::move(attempt.reservation)}};
}

resource_manager::reservation_result<resource_manager::session_reservation>
resource_manager::reserve_session(session_direction direction) noexcept {
   if (!state_) {
      return reservation_result<session_reservation>{transition_result::invalid_transition};
   }
   auto attempt = state_->reserve_session(direction);
   if (!attempt.reservation) {
      return reservation_result<session_reservation>{attempt.outcome};
   }
   return reservation_result<session_reservation>{session_reservation{state_, std::move(attempt.reservation)}};
}

resource_manager::reservation_result<resource_manager::dial_reservation> resource_manager::reserve_dial() noexcept {
   if (!state_) {
      return reservation_result<dial_reservation>{transition_result::invalid_transition};
   }
   auto attempt = state_->reserve_dial();
   if (!attempt.reservation) {
      return reservation_result<dial_reservation>{attempt.outcome};
   }
   return reservation_result<dial_reservation>{dial_reservation{state_, std::move(attempt.reservation)}};
}

resource_manager::reservation_result<resource_manager::dial_reservation>
resource_manager::reserve_dial(peer_id peer) noexcept {
   if (!state_) {
      return reservation_result<dial_reservation>{transition_result::invalid_transition};
   }
   auto attempt = state_->reserve_dial(std::move(peer));
   if (!attempt.reservation) {
      return reservation_result<dial_reservation>{attempt.outcome};
   }
   return reservation_result<dial_reservation>{dial_reservation{state_, std::move(attempt.reservation)}};
}

resource_manager::reservation_result<resource_manager::stream_reservation>
resource_manager::reserve_stream(peer_id peer, session_direction direction) noexcept {
   if (!state_) {
      return reservation_result<stream_reservation>{transition_result::invalid_transition};
   }
   auto attempt = state_->reserve_stream(std::move(peer), direction);
   if (!attempt.reservation) {
      return reservation_result<stream_reservation>{attempt.outcome};
   }
   return reservation_result<stream_reservation>{stream_reservation{state_, std::move(attempt.reservation)}};
}

resource_manager::reservation_result<resource_manager::relay_reservation>
resource_manager::reserve_relay(peer_id peer) noexcept {
   if (!state_) {
      return reservation_result<relay_reservation>{transition_result::invalid_transition};
   }
   const auto outcome = state_->reserve_relay(peer);
   if (outcome != transition_result::accepted) {
      return reservation_result<relay_reservation>{outcome};
   }
   return reservation_result<relay_reservation>{relay_reservation{state_, std::move(peer)}};
}

resource_manager::reservation_result<resource_manager::relay_reservation>
resource_manager::reserve_relay(scope value) noexcept {
   return reserve_relay(std::move(value.peer));
}

resource_manager::transition_result resource_manager::record_malformed(peer_id peer) noexcept {
   return state_ ? state_->record_malformed(peer) : transition_result::invalid_transition;
}

resource_manager::transition_result resource_manager::record_malformed(scope value) noexcept {
   return record_malformed(std::move(value.peer));
}

resource_manager::lifecycle_reservation::lifecycle_reservation() noexcept = default;

resource_manager::lifecycle_reservation::lifecycle_reservation(std::shared_ptr<state> owner,
                                                               std::shared_ptr<ledger> ledger) noexcept
    : owner_(std::move(owner)), ledger_(std::move(ledger)) {}

resource_manager::lifecycle_reservation::~lifecycle_reservation() {
   release();
}

resource_manager::lifecycle_reservation::lifecycle_reservation(lifecycle_reservation&& other) noexcept
    : owner_(std::move(other.owner_)), ledger_(std::move(other.ledger_)) {}

resource_manager::lifecycle_reservation&
resource_manager::lifecycle_reservation::operator=(lifecycle_reservation&& other) noexcept {
   if (this != &other) {
      release();
      owner_ = std::move(other.owner_);
      ledger_ = std::move(other.ledger_);
   }
   return *this;
}

bool resource_manager::lifecycle_reservation::active() const noexcept {
   return owner_ != nullptr && ledger_ != nullptr;
}

std::optional<resource_manager::memory_reservation>
resource_manager::lifecycle_reservation::reserve_memory(std::uint64_t bytes, memory_priority priority) noexcept {
   if (!active() || !owner_->reserve_memory(ledger_, bytes, priority)) {
      return std::nullopt;
   }
   return memory_reservation{owner_, ledger_, bytes};
}

std::optional<resource_manager::file_descriptor_reservation>
resource_manager::lifecycle_reservation::reserve_file_descriptors(std::size_t count) noexcept {
   if (!active() || !owner_->reserve_file_descriptors(ledger_, count)) {
      return std::nullopt;
   }
   return file_descriptor_reservation{owner_, ledger_, count};
}

void resource_manager::lifecycle_reservation::release() noexcept {
   if (owner_ && ledger_) {
      owner_->release_parent(ledger_);
   }
   ledger_.reset();
   owner_.reset();
}

resource_manager::session_reservation::session_reservation() noexcept = default;

resource_manager::session_reservation::session_reservation(std::shared_ptr<state> owner,
                                                           std::shared_ptr<ledger> ledger) noexcept
    : owner_(std::move(owner)), ledger_(std::move(ledger)) {}

resource_manager::session_reservation::~session_reservation() {
   release();
}

resource_manager::session_reservation::session_reservation(session_reservation&& other) noexcept
    : owner_(std::move(other.owner_)), ledger_(std::move(other.ledger_)) {}

resource_manager::session_reservation&
resource_manager::session_reservation::operator=(session_reservation&& other) noexcept {
   if (this != &other) {
      release();
      owner_ = std::move(other.owner_);
      ledger_ = std::move(other.ledger_);
   }
   return *this;
}

bool resource_manager::session_reservation::active() const noexcept {
   return owner_ != nullptr && ledger_ != nullptr;
}

bool resource_manager::session_reservation::established() const noexcept {
   return active() && owner_->session_established(ledger_);
}

resource_manager::transition_result resource_manager::session_reservation::establish(session_scope value) noexcept {
   return active() ? owner_->establish_session(ledger_, std::move(value)) : transition_result::invalid_transition;
}

std::optional<resource_manager::memory_reservation>
resource_manager::session_reservation::reserve_memory(std::uint64_t bytes, memory_priority priority) noexcept {
   if (!active() || !owner_->reserve_memory(ledger_, bytes, priority)) {
      return std::nullopt;
   }
   return memory_reservation{owner_, ledger_, bytes};
}

std::optional<resource_manager::file_descriptor_reservation>
resource_manager::session_reservation::reserve_file_descriptors(std::size_t count) noexcept {
   if (!active() || !owner_->reserve_file_descriptors(ledger_, count)) {
      return std::nullopt;
   }
   return file_descriptor_reservation{owner_, ledger_, count};
}

void resource_manager::session_reservation::release() noexcept {
   if (owner_ && ledger_) {
      owner_->release_parent(ledger_);
   }
   ledger_.reset();
   owner_.reset();
}

resource_manager::dial_reservation::dial_reservation() noexcept = default;

resource_manager::dial_reservation::dial_reservation(std::shared_ptr<state> owner,
                                                     std::shared_ptr<dial_ledger> ledger) noexcept
    : owner_(std::move(owner)), ledger_(std::move(ledger)) {}

resource_manager::dial_reservation::~dial_reservation() {
   release();
}

resource_manager::dial_reservation::dial_reservation(dial_reservation&& other) noexcept
    : owner_(std::move(other.owner_)), ledger_(std::move(other.ledger_)) {}

resource_manager::dial_reservation& resource_manager::dial_reservation::operator=(dial_reservation&& other) noexcept {
   if (this != &other) {
      release();
      owner_ = std::move(other.owner_);
      ledger_ = std::move(other.ledger_);
   }
   return *this;
}

bool resource_manager::dial_reservation::active() const noexcept {
   return owner_ && owner_->dial_active(ledger_);
}

bool resource_manager::dial_reservation::bound() const noexcept {
   return owner_ && owner_->dial_bound(ledger_);
}

resource_manager::transition_result resource_manager::dial_reservation::bind(peer_id peer) noexcept {
   return owner_ ? owner_->bind_dial(ledger_, std::move(peer)) : transition_result::invalid_transition;
}

void resource_manager::dial_reservation::release() noexcept {
   if (owner_) {
      owner_->release_dial(ledger_);
   }
   ledger_.reset();
   owner_.reset();
}

resource_manager::stream_reservation::stream_reservation() noexcept = default;

resource_manager::stream_reservation::stream_reservation(std::shared_ptr<state> owner,
                                                         std::shared_ptr<ledger> ledger) noexcept
    : owner_(std::move(owner)), ledger_(std::move(ledger)) {}

resource_manager::stream_reservation::~stream_reservation() {
   release();
}

resource_manager::stream_reservation::stream_reservation(stream_reservation&& other) noexcept
    : owner_(std::move(other.owner_)), ledger_(std::move(other.ledger_)) {}

resource_manager::stream_reservation&
resource_manager::stream_reservation::operator=(stream_reservation&& other) noexcept {
   if (this != &other) {
      release();
      owner_ = std::move(other.owner_);
      ledger_ = std::move(other.ledger_);
   }
   return *this;
}

bool resource_manager::stream_reservation::active() const noexcept {
   return owner_ != nullptr && ledger_ != nullptr;
}

bool resource_manager::stream_reservation::bound() const noexcept {
   return active() && owner_->stream_bound(ledger_);
}

bool resource_manager::stream_reservation::service_bound() const noexcept {
   return active() && owner_->stream_service_bound(ledger_);
}

resource_manager::stream_reservation::bind_result
resource_manager::stream_reservation::bind_protocol(const protocol_id& value) noexcept {
   return active() ? owner_->bind_protocol(ledger_, value) : bind_result::invalid_transition;
}

resource_manager::stream_reservation::bind_result
resource_manager::stream_reservation::bind_service(std::string_view value) noexcept {
   return active() ? owner_->bind_service(ledger_, value) : bind_result::invalid_transition;
}

std::optional<resource_manager::memory_reservation>
resource_manager::stream_reservation::reserve_memory(std::uint64_t bytes, memory_priority priority) noexcept {
   if (!active()) {
      wlog("forge.net.p2p.resources", "P2P memory reservation rejected",
           ("event", "p2p.memory_reservation_failed")("reason", "inactive_reservation")("scope", "reservation")(
               "requested_bytes", bytes)("priority", static_cast<unsigned>(priority)));
      return std::nullopt;
   }
   if (!owner_->reserve_memory(ledger_, bytes, priority)) {
      return std::nullopt;
   }
   return memory_reservation{owner_, ledger_, bytes};
}

std::optional<resource_manager::file_descriptor_reservation>
resource_manager::stream_reservation::reserve_file_descriptors(std::size_t count) noexcept {
   if (!active() || !owner_->reserve_file_descriptors(ledger_, count)) {
      return std::nullopt;
   }
   return file_descriptor_reservation{owner_, ledger_, count};
}

void resource_manager::stream_reservation::release() noexcept {
   if (owner_ && ledger_) {
      owner_->release_parent(ledger_);
   }
   ledger_.reset();
   owner_.reset();
}

resource_manager::relay_reservation::relay_reservation() noexcept = default;

resource_manager::relay_reservation::relay_reservation(std::shared_ptr<state> owner, peer_id peer) noexcept
    : owner_(std::move(owner)), peer_(std::move(peer)) {}

resource_manager::relay_reservation::~relay_reservation() {
   release();
}

resource_manager::relay_reservation::relay_reservation(relay_reservation&& other) noexcept
    : owner_(std::move(other.owner_)), peer_(std::move(other.peer_)) {}

resource_manager::relay_reservation&
resource_manager::relay_reservation::operator=(relay_reservation&& other) noexcept {
   if (this != &other) {
      release();
      owner_ = std::move(other.owner_);
      peer_ = std::move(other.peer_);
   }
   return *this;
}

bool resource_manager::relay_reservation::active() const noexcept {
   return owner_ != nullptr;
}

void resource_manager::relay_reservation::release() noexcept {
   if (owner_) {
      owner_->release_relay(peer_);
   }
   owner_.reset();
}

resource_manager::memory_reservation::memory_reservation() noexcept = default;

resource_manager::memory_reservation::memory_reservation(std::shared_ptr<state> owner, std::shared_ptr<ledger> ledger,
                                                         std::uint64_t bytes) noexcept
    : owner_(std::move(owner)), ledger_(std::move(ledger)), bytes_(bytes) {}

resource_manager::memory_reservation::~memory_reservation() {
   release();
}

resource_manager::memory_reservation::memory_reservation(memory_reservation&& other) noexcept
    : owner_(std::move(other.owner_)), ledger_(std::move(other.ledger_)), bytes_(std::exchange(other.bytes_, 0)) {}

resource_manager::memory_reservation&
resource_manager::memory_reservation::operator=(memory_reservation&& other) noexcept {
   if (this != &other) {
      release();
      owner_ = std::move(other.owner_);
      ledger_ = std::move(other.ledger_);
      bytes_ = std::exchange(other.bytes_, 0);
   }
   return *this;
}

bool resource_manager::memory_reservation::active() const noexcept {
   return owner_ != nullptr && ledger_ != nullptr;
}

std::uint64_t resource_manager::memory_reservation::bytes() const noexcept {
   return bytes_;
}

void resource_manager::memory_reservation::release() noexcept {
   if (owner_ && ledger_) {
      owner_->release_memory(ledger_, bytes_);
   }
   ledger_.reset();
   owner_.reset();
   bytes_ = 0;
}

resource_manager::file_descriptor_reservation::file_descriptor_reservation() noexcept = default;

resource_manager::file_descriptor_reservation::file_descriptor_reservation(std::shared_ptr<state> owner,
                                                                           std::shared_ptr<ledger> ledger,
                                                                           std::size_t count) noexcept
    : owner_(std::move(owner)), ledger_(std::move(ledger)), count_(count) {}

resource_manager::file_descriptor_reservation::~file_descriptor_reservation() {
   release();
}

resource_manager::file_descriptor_reservation::file_descriptor_reservation(file_descriptor_reservation&& other) noexcept
    : owner_(std::move(other.owner_)), ledger_(std::move(other.ledger_)), count_(std::exchange(other.count_, 0)) {}

resource_manager::file_descriptor_reservation&
resource_manager::file_descriptor_reservation::operator=(file_descriptor_reservation&& other) noexcept {
   if (this != &other) {
      release();
      owner_ = std::move(other.owner_);
      ledger_ = std::move(other.ledger_);
      count_ = std::exchange(other.count_, 0);
   }
   return *this;
}

bool resource_manager::file_descriptor_reservation::active() const noexcept {
   return owner_ != nullptr && ledger_ != nullptr;
}

std::size_t resource_manager::file_descriptor_reservation::count() const noexcept {
   return count_;
}

void resource_manager::file_descriptor_reservation::release() noexcept {
   if (owner_ && ledger_) {
      owner_->release_file_descriptors(ledger_, count_);
   }
   ledger_.reset();
   owner_.reset();
   count_ = 0;
}

} // namespace forge::net::p2p
