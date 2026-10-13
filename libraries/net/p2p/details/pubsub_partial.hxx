#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

namespace forge::net::p2p::detail {

// Local registration and callback accounting only. No transport, reconstruction or peer scoring.
class pubsub_partial {
   struct state;

 public:
   struct registration {
      pubsub::partial_topic token;
      pubsub::partial_options options;
      std::stop_source stop;
      std::uint64_t generation = 0;
   };

   class callback {
    public:
      ~callback();
      callback(const callback&) = delete;
      callback& operator=(const callback&) = delete;
      [[nodiscard]] std::vector<std::vector<std::uint8_t>> take_groups() noexcept;

    private:
      friend class pubsub_partial;
      callback(std::shared_ptr<state> owner, std::shared_ptr<registration> registration,
               std::size_t bytes, bool gossip);
      std::shared_ptr<state> _owner;
      std::shared_ptr<registration> _registration;
      std::size_t _bytes;
      bool _gossip;
      bool _armed = false;
      std::vector<std::vector<std::uint8_t>> _groups;
   };

   pubsub_partial();
   [[nodiscard]] std::shared_ptr<registration> prepare(pubsub::topic subject, pubsub::partial_options options);
   // Caller serializes installation with node subscription state. Returned old registration must be stopped outside locks.
   [[nodiscard]] std::shared_ptr<registration> install(const std::shared_ptr<registration>& value,
                                                      const pubsub::limits& limits);
   [[nodiscard]] std::shared_ptr<registration> find(const pubsub::topic& subject) const;
   [[nodiscard]] std::shared_ptr<registration> require(const pubsub::partial_topic& token) const;
   [[nodiscard]] bool current(const pubsub::partial_topic& token) const noexcept;
   [[nodiscard]] std::shared_ptr<registration> close(const pubsub::topic& subject);
   // Validates and removes exactly this owner/topic/generation in one registry transaction.
   [[nodiscard]] std::shared_ptr<registration> close(const pubsub::partial_topic& token);
   void stop() noexcept;
   void advertise(const pubsub::partial_topic& token, std::vector<std::uint8_t> group, const pubsub::limits& limits);
   void forget(const pubsub::partial_topic& token, const std::vector<std::uint8_t>& group);
   void heartbeat() noexcept;
   [[nodiscard]] std::vector<std::shared_ptr<registration>> registrations() const;
   [[nodiscard]] std::shared_ptr<callback> admit(const std::shared_ptr<registration>& value,
                                               std::size_t bytes, bool gossip, const pubsub::limits& limits);
   void failed() noexcept;
   void snapshot(pubsub::snapshot& out) const noexcept;

 private:
   struct state {
      struct entry {
         std::shared_ptr<registration> value;
         std::map<std::vector<std::uint8_t>, std::size_t> groups;
         std::size_t bytes = 0;
      };
      std::mutex mutex;
      std::map<std::string, entry> topics;
      std::set<std::string> active_gossip; // Survives registration replacement until the owning lease finishes.
      std::uint64_t next = 1;
      std::size_t groups = 0, group_bytes = 0, callbacks = 0, callback_bytes = 0;
      std::uint64_t failures = 0, rejections = 0;
      bool closed = false;
   };
   std::shared_ptr<state> _state;
};

} // namespace forge::net::p2p::detail
