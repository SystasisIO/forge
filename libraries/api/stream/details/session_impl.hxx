#pragma once

namespace forge::api::stream {

struct session::impl final : std::enable_shared_from_this<session::impl> {
   using strand_type = boost::asio::strand<boost::asio::any_io_executor>;
   using timer = boost::asio::steady_timer;

   struct write_receipt {
      explicit write_receipt(const strand_type& executor);

      timer wake;
      std::exception_ptr error;
      bool done = false;
   };

   struct queued_frame {
      forge::api::core::frame value;
      std::shared_ptr<write_receipt> receipt;
      std::size_t buffered_item_bytes = 0;
      bool buffered_item = false;
   };

   struct flow_state {
      forge::api::core::stream_direction direction = forge::api::core::stream_direction::input;
      std::shared_ptr<forge::api::core::detail::stream_endpoint> endpoint;
      std::uint64_t transferred_items = 0;
      std::uint64_t transferred_bytes = 0;
      std::uint64_t limit_items = 0;
      std::uint64_t limit_bytes = 0;
      std::uint64_t buffered_items = 0;
      std::uint64_t buffered_bytes = 0;
      std::deque<std::vector<std::uint8_t>> pending_items;
      bool ended = false;
      bool discarding = false;
      bool pump_started = false;
      bool pump_done = false;
   };

   struct tombstone_flow {
      forge::api::core::stream_direction direction = forge::api::core::stream_direction::input;
      std::uint64_t transferred_items = 0;
      std::uint64_t transferred_bytes = 0;
      std::uint64_t limit_items = 0;
      std::uint64_t limit_bytes = 0;
      bool ended = false;
   };

   struct tombstone_state {
      forge::api::core::api_ref api;
      std::string method;
      forge::api::core::codec_id codec;
      std::optional<tombstone_flow> inbound;
   };

   struct call_state {
      call_state(const strand_type& executor, forge::api::core::call_id value,
                 forge::api::core::method_kind method_kind, bool local_origin_value);

      forge::api::core::call_id id;
      forge::api::core::method_kind kind;
      forge::api::core::api_ref api;
      std::string method;
      forge::api::core::codec_id codec;
      std::optional<forge::api::core::method_descriptor> descriptor;
      std::optional<forge::api::core::pinned_binding_plan> selection;
      std::uint64_t admission_order = 0;
      bool local_origin = false;
      timer wake;
      timer deadline;
      boost::asio::cancellation_signal handler_cancel;
      std::optional<flow_state> inbound;
      std::optional<flow_state> outbound;
      std::deque<queued_frame> write_queue;
      std::optional<forge::api::core::frame> terminal;
      std::exception_ptr error;
      bool request_written = false;
      bool terminal_enqueued = false;
      bool terminal_written = false;
      bool handler_running = false;
      bool handler_done = false;
      std::uint64_t writes_in_flight = 0;
      bool done = false;
      bool rr_queued = false;
   };

   impl(forge::net::transport::stream stream_value, options settings_value);
   impl(forge::net::transport::stream stream_value, forge::api::core::binding_plan plan_value, options settings_value,
        forge::api::core::dispatch_options dispatch_options,
        std::optional<forge::api::core::trusted_invocation> trusted = std::nullopt);

   [[nodiscard]] bool valid() const noexcept;
   [[nodiscard]] strand_type ensure_strand(boost::asio::any_io_executor executor);
   [[nodiscard]] std::optional<strand_type> current_strand() const;
   void initialize_on_strand(const strand_type& executor);
   void validate_options() const;

   [[nodiscard]] forge::api::core::session_hello local_hello() const;
   void negotiate_hello(const forge::api::core::session_hello& peer);
   void throw_if_terminated_on_strand(const std::shared_ptr<call_state>& call = {}) const;
   boost::asio::awaitable<void> ensure_handshake_on_strand(const std::shared_ptr<call_state>& call = {});
   boost::asio::awaitable<void> wait_receipt_on_strand(const std::shared_ptr<write_receipt>& receipt);

   [[nodiscard]] std::vector<std::uint8_t> encode_wire_frame(const forge::api::core::frame& value) const;
   [[nodiscard]] forge::api::core::frame decode_wire_frame(forge::net::transport::chunk payload) const;
   [[nodiscard]] forge::api::core::session_hello decode_hello(const forge::api::core::frame& value) const;
   [[nodiscard]] forge::api::core::stream_window decode_window(const forge::api::core::frame& value) const;
   [[nodiscard]] forge::api::core::stream_end decode_end(const forge::api::core::frame& value) const;

   boost::asio::awaitable<void> reader_loop();
   boost::asio::awaitable<forge::net::transport::chunk> read_wire_frame();
   boost::asio::awaitable<void> handle_inbound_frame(forge::api::core::frame value);
   boost::asio::awaitable<void> handle_request(forge::api::core::frame value);
   void handle_stream_item(const std::shared_ptr<call_state>& call, forge::api::core::frame value);
   void handle_stream_end(const std::shared_ptr<call_state>& call, const forge::api::core::frame& value);
   void handle_stream_window(const std::shared_ptr<call_state>& call, const forge::api::core::frame& value);
   void handle_terminal(const std::shared_ptr<call_state>& call, forge::api::core::frame value);
   void handle_cancel(const std::shared_ptr<call_state>& call);
   void handle_tombstone_frame(tombstone_state& tombstone, const forge::api::core::frame& value);

   boost::asio::awaitable<void> writer_loop();
   [[nodiscard]] std::optional<queued_frame> next_write_on_strand();
   void enqueue_control(forge::api::core::frame value);
   std::shared_ptr<write_receipt> enqueue_call_frame(const std::shared_ptr<call_state>& call,
                                                     forge::api::core::frame value);
   void wake_writer() noexcept;
   void wake_session() noexcept;
   void touch_activity() noexcept;
   boost::asio::awaitable<void> idle_watchdog();
   void wake_call(const std::shared_ptr<call_state>& call) noexcept;
   void complete_receipt(const std::shared_ptr<write_receipt>& receipt, std::exception_ptr error = {}) noexcept;
   void release_outbound_capacity(const queued_frame& value) noexcept;
   void wake_outbound_capacity() noexcept;
   [[nodiscard]] std::uint64_t outbound_item_limit() const noexcept;

   boost::asio::awaitable<forge::api::core::frame>
   async_call_on_strand(forge::api::core::frame request, forge::api::core::method_kind kind,
                        std::shared_ptr<forge::api::core::detail::stream_endpoint> input,
                        std::shared_ptr<forge::api::core::detail::stream_endpoint> output, call_options value,
                        const forge::api::core::method_descriptor* descriptor);
   [[nodiscard]] std::shared_ptr<call_state>
   reserve_local_call(forge::api::core::frame& request, forge::api::core::method_kind kind,
                      std::shared_ptr<forge::api::core::detail::stream_endpoint> input,
                      std::shared_ptr<forge::api::core::detail::stream_endpoint> output, call_options& value,
                      const forge::api::core::method_descriptor* descriptor);
   [[nodiscard]] std::shared_ptr<call_state> make_remote_call(const forge::api::core::frame& request,
                                                              forge::api::core::method_kind kind,
                                                              const forge::api::core::method_descriptor* descriptor,
                                                              forge::api::core::pinned_binding_plan selection);
   boost::asio::awaitable<void> run_remote_call(forge::api::core::frame request,
                                                const std::shared_ptr<call_state>& call);
   boost::asio::awaitable<void> pump_outbound(const std::shared_ptr<call_state>& call);
   void start_outbound_pump(const std::shared_ptr<call_state>& call);
   boost::asio::awaitable<void> pump_inbound(const std::shared_ptr<call_state>& call);
   void start_inbound_pump(const std::shared_ptr<call_state>& call);
   void finish_unstarted_pumps(const std::shared_ptr<call_state>& call) noexcept;
   boost::asio::awaitable<void> wait_for_credit(const std::shared_ptr<call_state>& call, std::size_t item_bytes);
   boost::asio::awaitable<void> wait_for_outbound_capacity(const std::shared_ptr<call_state>& call,
                                                           std::size_t item_bytes);
   boost::asio::awaitable<void> wait_for_terminal(const std::shared_ptr<call_state>& call);
   boost::asio::awaitable<void> wait_for_outbound_pump(const std::shared_ptr<call_state>& call);
   void start_deadline(const std::shared_ptr<call_state>& call, std::chrono::milliseconds value);
   void cancel_call(const std::shared_ptr<call_state>& call, std::exception_ptr error, bool notify_peer);
   void discard_inbound(const std::shared_ptr<call_state>& call) noexcept;
   void finish_call(const std::shared_ptr<call_state>& call);
   void remember_tombstone(const std::shared_ptr<call_state>& call);
   void complete_tombstone(std::uint64_t id);
   [[nodiscard]] std::size_t draining_tombstones() const noexcept;
   void install_inbound_observer(const std::shared_ptr<call_state>& call);
   void on_inbound_event(std::uint64_t id, forge::api::core::detail::stream_event event, std::size_t bytes);
   void replenish_inbound_credit();
   [[nodiscard]] std::uint64_t aggregate_buffered_bytes() const noexcept;
   [[nodiscard]] std::uint64_t aggregate_outstanding_credit() const noexcept;

   boost::asio::awaitable<void> async_serve_on_strand();
   boost::asio::awaitable<void> async_close_on_strand();
   void fail_session(std::exception_ptr error) noexcept;
   void stop_transport() noexcept;
   [[nodiscard]] bool writer_idle() const noexcept;

   forge::net::transport::stream stream;
   options settings;
   std::optional<forge::api::core::binding_plan> plan;
   std::optional<forge::api::core::frame_dispatcher> dispatcher;
   forge::api::core::session_limits negotiated_limits;
   forge::api::core::capability_set negotiated_capabilities;
   std::vector<std::uint8_t> read_buffer;
   std::size_t read_consumed = 0;
   std::unordered_map<std::uint64_t, std::shared_ptr<call_state>> calls;
   std::unordered_map<std::uint64_t, tombstone_state> tombstones;
   std::deque<std::uint64_t> tombstone_order;
   std::deque<queued_frame> control_queue;
   std::deque<std::uint64_t> round_robin;
   std::uint64_t next_call_id = 1;
   std::uint64_t next_remote_call_id = std::uint64_t{1} << 63U;
   std::uint64_t next_admission_order = 1;
   std::uint64_t outbound_buffered_items = 0;
   std::uint64_t outbound_buffered_bytes = 0;
   std::size_t control_burst = 0;
   std::exception_ptr failure;
   mutable std::mutex executor_mutex;
   std::optional<strand_type> strand;
   forge::asio::notification session_wake;
   std::shared_ptr<timer> writer_wake;
   std::shared_ptr<timer> idle_timer;
   std::atomic_bool closed{false};
   bool initialized = false;
   bool accepting = true;
   bool closing = false;
   bool reader_running = false;
   bool writer_running = false;
   bool writer_write_in_flight = false;
   bool hello_sent = false;
   bool peer_hello_received = false;
};

} // namespace forge::api::stream
