#pragma once

namespace forge::plugins::net::p2p::node::test {

class protocol_registry_fixture {
 public:
   enum class point { none, before, after };
   using install_failure = std::runtime_error;
   using remove_failure = std::logic_error;

   class job {
    public:
      job(protocol_registry_fixture& fixture, std::function<void()> body);
      ~job();
      job(const job&) = delete;
      job& operator=(const job&) = delete;
      void finish();
    private:
      protocol_registry_fixture& fixture_;
      std::future<void> done_;
      std::thread thread_;
   };

   explicit protocol_registry_fixture(std::size_t max_slots = 1);
   ~protocol_registry_fixture();
   void fail_install(point value);
   void fail_remove(point value);
   void hold_install(point value);
   void wait_install();
   void release_install() noexcept;
   [[nodiscard]] bool contains(const forge::net::p2p::protocol_id& protocol) const;
   [[nodiscard]] std::size_t installs() const;
   [[nodiscard]] std::size_t removals() const;
   [[nodiscard]] std::optional<bool> last_remove_result() const;
   [[nodiscard]] forge::api::p2p::publication publish(forge::net::p2p::protocol_id protocol);
   void run(boost::asio::awaitable<void> operation);
   [[nodiscard]] static forge::net::p2p::node::protocol_handler handler();

   std::shared_ptr<detail::protocol_registry> owner;
   std::shared_ptr<const detail::protocol_operations> operations;

 private:
   struct state {
      mutable std::mutex mutex;
      std::condition_variable ready;
      std::map<forge::net::p2p::protocol_id, forge::net::p2p::node::protocol_handler> routes;
      point install_error = point::none;
      point remove_error = point::none;
      point pause = point::none;
      bool entered = false;
      bool released = false;
      std::size_t installs = 0;
      std::size_t removals = 0;
      std::optional<bool> remove_result;
   };
   static void install(const std::shared_ptr<state>& state, forge::net::p2p::protocol_id protocol,
                       forge::net::p2p::node::protocol_handler handler);
   static bool remove(const std::shared_ptr<state>& state, const forge::net::p2p::protocol_id& protocol);
   static void pause(const std::shared_ptr<state>& state, point value);
   static void wait(std::future<void>& done);
   std::shared_ptr<state> state_;
   forge::asio::runtime runtime_;
   forge::api::core::registry apis_;
};

}
