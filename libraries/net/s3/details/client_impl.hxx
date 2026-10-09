#pragma once

namespace forge::net::s3 {

struct client::impl : std::enable_shared_from_this<client::impl> {
   struct backend;

   struct activity {
      std::shared_ptr<impl> owner;
      request_options options;
      std::stop_token worker_stop;
      std::chrono::steady_clock::time_point deadline;
      bool mutating;
      std::atomic<bool> started{false};
      std::atomic<bool> response_exceeded{false};

      activity(std::shared_ptr<impl> owner, request_options options, bool mutating);
      ~activity();
      [[nodiscard]] bool interrupted() const noexcept;
      void check() const;
   };

   impl(asio::compute::executor executor, config options);
   ~impl();
   void update(credentials identity);
   void stop() noexcept;
   static boost::asio::awaitable<void> drain(std::shared_ptr<impl> owner);

   metadata head(const object& target, activity& call);
   metadata put(const object& target, std::variant<std::vector<std::byte>, std::filesystem::path> source,
                const write_options& write, activity& call);
   std::vector<std::byte> get(const object& target, const read_options& read, activity& call);
   metadata get(const object& target, const std::filesystem::path& destination, const read_options& read,
                activity& call);
   void erase(const object& target, activity& call);
   signed_url presign(const object& target, std::chrono::seconds lifetime, method verb, activity& call);
   multipart begin(const object& target, const write_options& write, activity& call);
   part upload(const multipart& session, std::uint32_t number,
               std::variant<std::vector<std::byte>, std::filesystem::path> source, std::optional<byte_range> range,
               activity& call);
   part_page parts(const multipart& session, std::uint32_t after, std::size_t limit, activity& call);
   metadata complete(const multipart& session, std::vector<part> parts, const write_options& conditions,
                     activity& call);
   void abort(const multipart& session, activity& call);

   template <typename Work>
   static boost::asio::awaitable<std::invoke_result_t<Work, impl&, activity&>>
   run(std::shared_ptr<impl> owner, std::string name, bool mutating, request_options options, Work work) {
      using result_type = std::invoke_result_t<Work, impl&, activity&>;
      auto call = std::make_shared<activity>(owner, std::move(options), mutating);
      try {
         auto submitted = owner->_executor.try_submit(
             {std::move(name), {}},
             [owner, call, work = std::move(work)](asio::compute::context& context) mutable -> result_type {
                call->worker_stop = context.stop_token();
                call->check();
                return std::invoke(work, *owner, *call);
             });
         if (!submitted) {
            throw exceptions::busy{"S3 blocking executor is full"};
         }
         if constexpr (std::is_void_v<result_type>) {
            co_await std::move(*submitted).wait();
         } else {
            co_return co_await std::move(*submitted).wait();
         }
      } catch (const asio::exceptions::canceled&) {
         if (mutating && call->started.load()) {
            throw exceptions::unknown_outcome{
                "S3 mutation was interrupted; reconcile the object or upload before retry"};
         }
         throw exceptions::canceled{"S3 operation was canceled before a confirmed result"};
      } catch (const asio::exceptions::rejected&) {
         throw exceptions::stopped{"S3 blocking executor is stopped"};
      }
   }

 private:
   asio::compute::executor _executor;
   config _options;
   std::stop_source _stop;
   std::mutex _mutex;
   std::size_t _active = 0;
   bool _closed = false;
   bool _draining = false;
   std::shared_ptr<boost::asio::steady_timer> _drain;
   std::unique_ptr<backend> _backend;
};

} // namespace forge::net::s3
