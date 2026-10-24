#include <boost/test/unit_test.hpp>
#include <boost/scope/scope_exit.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <span>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

import forge.expression.worker_protocol;
import forge.expression.exceptions;
import forge.expression.artifact;

namespace {

namespace protocol = forge::expression::worker_protocol;
namespace expression = forge::expression;
using namespace std::chrono_literals;

enum class outcome { response, closed, timeout };

struct exchange_result {
   outcome state = outcome::closed;
   protocol::response reply;
   bool exhaustion_marker = false;
};

// Test-only direct transport. It exercises the actual worker with the SAME Raw
// codec while independently observing its allocation-failure endpoint and EOF.
class direct_worker {
 public:
   explicit direct_worker(std::size_t capacity = 64U * 1024U * 1024U) {
      std::array<int, 4> descriptors{-1, -1, -1, -1};
      const auto cleanup = boost::scope::scope_exit{[&] {
         for (const auto fd : descriptors) {
            if (fd >= 0) {
               ::close(fd);
            }
         }
      }};
      BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()), 0);
      BOOST_REQUIRE_EQUAL(::pipe(descriptors.data() + 2), 0);
      for (auto& fd : descriptors) {
         const auto duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 4);
         BOOST_REQUIRE_GE(duplicate, 4);
         ::close(fd);
         fd = duplicate;
      }
      const int no_sigpipe = 1;
      BOOST_REQUIRE_EQUAL(::setsockopt(descriptors[0], SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)), 0);
      BOOST_REQUIRE_EQUAL(::fcntl(descriptors[0], F_SETFL, O_NONBLOCK), 0);
      BOOST_REQUIRE_EQUAL(::fcntl(descriptors[2], F_SETFL, O_NONBLOCK), 0);
      BOOST_REQUIRE_EQUAL(::fcntl(descriptors[3], F_SETFL, O_NONBLOCK), 0);

      posix_spawn_file_actions_t actions;
      BOOST_REQUIRE_EQUAL(::posix_spawn_file_actions_init(&actions), 0);
      const auto destroy_actions = boost::scope::scope_exit{[&] { ::posix_spawn_file_actions_destroy(&actions); }};
      BOOST_REQUIRE_EQUAL(::posix_spawn_file_actions_adddup2(&actions, descriptors[1], 0), 0);
      BOOST_REQUIRE_EQUAL(::posix_spawn_file_actions_adddup2(&actions, descriptors[1], 1), 0);
      BOOST_REQUIRE_EQUAL(::posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0), 0);
      BOOST_REQUIRE_EQUAL(::posix_spawn_file_actions_adddup2(&actions, descriptors[3], 3), 0);
      posix_spawnattr_t attributes;
      BOOST_REQUIRE_EQUAL(::posix_spawnattr_init(&attributes), 0);
      const auto destroy_attributes = boost::scope::scope_exit{[&] { ::posix_spawnattr_destroy(&attributes); }};
      sigset_t mask;
      sigset_t defaults;
      sigemptyset(&mask);
      sigfillset(&defaults);
      BOOST_REQUIRE_EQUAL(::posix_spawnattr_setsigmask(&attributes, &mask), 0);
      BOOST_REQUIRE_EQUAL(::posix_spawnattr_setsigdefault(&attributes, &defaults), 0);
      BOOST_REQUIRE_EQUAL(::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK |
                                                                      POSIX_SPAWN_SETSIGDEF),
                          0);
      auto helper = std::string{FORGE_EXPRESSION_WORKER};
      auto cap = std::string{"FORGE_EXPRESSION_HEAP_BYTES="} + std::to_string(capacity);
      std::array<char*, 2> arguments{helper.data(), nullptr};
      std::array<char*, 5> environment{const_cast<char*>("LANG=C"), const_cast<char*>("LC_ALL=C"),
                                       const_cast<char*>("TZ=UTC"), cap.data(), nullptr};
      BOOST_REQUIRE_EQUAL(
          ::posix_spawn(&_pid, helper.c_str(), &actions, &attributes, arguments.data(), environment.data()), 0);
      _channel = std::exchange(descriptors[0], -1);
      _faults = std::exchange(descriptors[2], -1);
      _capacity = capacity;
   }

   ~direct_worker() {
      if (_channel >= 0) {
         ::close(_channel);
      }
      if (_faults >= 0) {
         ::close(_faults);
      }
      if (_pid > 0) {
         ::kill(_pid, SIGKILL);
         int status = 0;
         while (::waitpid(_pid, &status, 0) < 0 && errno == EINTR) {
         }
      }
   }

   exchange_result exchange(protocol::request request, const forge::variant_object& values = {},
                            std::chrono::milliseconds timeout = 15s) {
      (void)markers();
      request.sequence = ++_sequence;
      const auto size = protocol::encode_into(request, values, _buffer);
      std::array<std::uint8_t, protocol::prefix_bytes> prefix;
      protocol::encode_length(static_cast<std::uint32_t>(size), prefix);
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      for (const auto part : {std::span<std::uint8_t>{prefix}, std::span{_buffer}.first(size)}) {
         const auto state = transfer(true, part, deadline);
         if (state != outcome::response) {
            return {state, {}, markers()};
         }
      }
      auto state = transfer(false, prefix, deadline);
      if (state != outcome::response) {
         return {state, {}, markers()};
      }
      const auto response_size = protocol::decode_length(prefix);
      BOOST_REQUIRE_LE(response_size, protocol::frame_limit);
      state = transfer(false, std::span{_buffer}.first(response_size), deadline);
      if (state != outcome::response) {
         return {state, {}, markers()};
      }
      auto reply = protocol::decode_response(std::span{_buffer}.first(response_size));
      BOOST_REQUIRE_LE(reply.expression.size(), expression::max_expression_bytes);
      BOOST_REQUIRE_EQUAL(reply.sequence, request.sequence);
      BOOST_REQUIRE_EQUAL(reply.heap_cap, _capacity);
      BOOST_REQUIRE_LE(reply.heap_live, reply.heap_cap);
      BOOST_REQUIRE_LE(reply.heap_peak, reply.heap_cap);
      return {outcome::response, std::move(reply), markers()};
   }

   exchange_result command(protocol::operation operation) {
      protocol::request request;
      request.op = operation;
      return exchange(std::move(request));
   }

   exchange_result prepare(std::string source, std::vector<expression::variable> declarations = {}) {
      protocol::request request;
      request.op = protocol::operation::prepare;
      request.source = std::move(source);
      request.declarations = std::move(declarations);
      return exchange(std::move(request));
   }

   exchange_result evaluate(std::uint64_t id, const forge::variant_object& values = {}) {
      protocol::request request;
      request.op = protocol::operation::evaluate;
      request.prepared_id = id;
      return exchange(std::move(request), values);
   }

   exchange_result release(std::uint64_t id) {
      protocol::request request;
      request.op = protocol::operation::release;
      request.prepared_id = id;
      return exchange(std::move(request));
   }

   void signal(int signal) {
      BOOST_REQUIRE_EQUAL(::kill(_pid, signal), 0);
   }

   void await_stopped() {
      int status = 0;
      pid_t result;
      do {
         result = ::waitpid(_pid, &status, WUNTRACED);
      } while (result < 0 && errno == EINTR);
      BOOST_REQUIRE_EQUAL(result, _pid);
      BOOST_REQUIRE(WIFSTOPPED(status));
   }

 private:
   outcome transfer(bool writing, std::span<std::uint8_t> bytes, std::chrono::steady_clock::time_point deadline) {
      std::size_t offset = 0;
      while (offset != bytes.size()) {
         const auto remaining =
             std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
         if (remaining <= 0ms) {
            return outcome::timeout;
         }
         pollfd descriptor{_channel, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
         const auto ready = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
         if (ready < 0 && errno == EINTR) {
            continue;
         }
         if (ready <= 0) {
            return ready == 0 ? outcome::timeout : outcome::closed;
         }
         const auto count = writing ? ::write(_channel, bytes.data() + offset, bytes.size() - offset)
                                    : ::read(_channel, bytes.data() + offset, bytes.size() - offset);
         if (count > 0) {
            offset += static_cast<std::size_t>(count);
         } else if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
         } else {
            return outcome::closed;
         }
      }
      return outcome::response;
   }

   bool markers() {
      bool result = false;
      std::array<std::uint8_t, 256> input;
      for (;;) {
         const auto count = ::read(_faults, input.data(), input.size());
         if (count > 0) {
            result = true;
         } else if (count < 0 && errno == EINTR) {
            continue;
         } else {
            return result;
         }
      }
   }

   pid_t _pid = -1;
   int _channel = -1;
   int _faults = -1;
   std::size_t _capacity = 0;
   std::uint64_t _sequence = 0;
   std::array<std::uint8_t, protocol::frame_limit> _buffer;
};

void successful(const exchange_result& result) {
   BOOST_REQUIRE(result.state == outcome::response);
   BOOST_REQUIRE(result.reply.code == protocol::status::ok);
   BOOST_REQUIRE(!result.exhaustion_marker);
}

bool allocation_failure(const exchange_result& result) {
   if (result.state == outcome::response) {
      return result.reply.code == protocol::status::resource_exhausted && result.exhaustion_marker &&
             result.reply.heap_failures != 0;
   }
   return result.state == outcome::closed && result.exhaustion_marker;
}

std::size_t open_descriptors() {
   std::size_t count = 0;
   for (int descriptor = 0; descriptor < 1024; ++descriptor) {
      count += (::fcntl(descriptor, F_GETFD) >= 0 || errno != EBADF) ? 1 : 0;
   }
   return count;
}

std::string balanced_sum(std::size_t leaves) {
   if (leaves == 1) {
      return "1";
   }
   return "(" + balanced_sum(leaves / 2) + "+" + balanced_sum(leaves - leaves / 2) + ")";
}

std::vector<expression::variable> variables(expression::type kind) {
   std::vector<expression::variable> result;
   for (int index = 0; index < 64; ++index) {
      result.push_back({"v" + std::to_string(index), kind});
   }
   return result;
}

forge::variant_object map_activation() {
   forge::mutable_variant_object result;
   result.reserve(64);
   for (int index = 0; index < 64; ++index) {
      forge::mutable_variant_object values;
      values.reserve(90);
      for (int key = 0; key < 90; ++key) {
         values.set(std::to_string(key), std::int64_t{key});
      }
      result.set("v" + std::to_string(index), forge::variant{forge::variant_object{std::move(values)}});
   }
   return forge::variant_object{std::move(result)};
}

} // namespace

BOOST_AUTO_TEST_CASE(expression_worker_transport_overflow_keeps_expression_category) {
   forge::mutable_variant_object fields;
   for (int field = 0; field < 64; ++field) {
      forge::mutable_variant_object elements;
      for (int key = 0; key < 256; ++key) {
         elements.set(std::string(60, 'k') + std::to_string(key), std::int64_t{key});
      }
      fields.set("v" + std::to_string(field), forge::variant{forge::variant_object{std::move(elements)}});
   }
   protocol::request request;
   request.sequence = 1;
   request.op = protocol::operation::evaluate;
   std::array<std::uint8_t, protocol::frame_limit> frame;
   const forge::variant_object values{std::move(fields)};
   BOOST_CHECK_THROW(protocol::encode_into(request, values, frame), expression::exceptions::resource_exhausted);
}

BOOST_AUTO_TEST_CASE(expression_worker_slots_release_and_identity) {
   direct_worker worker;
   successful(worker.command(protocol::operation::hello));
   std::array<std::uint64_t, 64> ids;
   for (auto& id : ids) {
      const auto prepared = worker.prepare("1");
      successful(prepared);
      id = prepared.reply.prepared_id;
      BOOST_REQUIRE_NE(id, 0U);
   }
   const auto full = worker.prepare("1");
   BOOST_REQUIRE(full.state == outcome::response);
   BOOST_CHECK(full.reply.code == protocol::status::resource_exhausted);
   BOOST_CHECK(!full.exhaustion_marker);
   const auto retained = full.reply.heap_live;
   const auto released = worker.release(ids[0]);
   successful(released);
   BOOST_CHECK_LT(released.reply.heap_live, retained);
   const auto replacement = worker.prepare("2");
   successful(replacement);
   BOOST_CHECK_GT(replacement.reply.prepared_id, ids.back());
   const auto stale = worker.evaluate(ids[0]);
   BOOST_REQUIRE(stale.state == outcome::response);
   BOOST_CHECK(stale.reply.code == protocol::status::invalid_profile);
   const auto evaluated = worker.evaluate(replacement.reply.prepared_id);
   successful(evaluated);
   BOOST_CHECK_EQUAL(evaluated.reply.i64, 2);
   for (std::size_t index = 1; index < ids.size(); ++index) {
      successful(worker.release(ids[index]));
   }
   successful(worker.release(replacement.reply.prepared_id));
   successful(worker.command(protocol::operation::stop));
}

BOOST_AUTO_TEST_CASE(expression_worker_real_preparation_allocation_failure) {
   const auto source = balanced_sum(256); // 511 actual CEL nodes, depth < 48.
   const auto declarations = variables(expression::type::int64);
   bool failed_allocation = false;
   for (const std::size_t cap :
        {256U * 1024U, 512U * 1024U, 1024U * 1024U, 2U * 1024U * 1024U, 4U * 1024U * 1024U, 8U * 1024U * 1024U}) {
      direct_worker worker(cap);
      const auto hello = worker.command(protocol::operation::hello);
      if (hello.state != outcome::response || hello.reply.code != protocol::status::ok) {
         continue; // Bootstrap refusal is not preparation failure evidence.
      }
      BOOST_REQUIRE_GT(hello.reply.heap_cap - hello.reply.heap_live, protocol::frame_limit);
      const auto prepared = worker.prepare(source, declarations);
      if (allocation_failure(prepared)) {
         failed_allocation = true;
         BOOST_TEST_MESSAGE("actual CEL preparation allocation refusal at cap=" << cap);
      } else {
         successful(prepared);
         const auto evaluated = worker.evaluate(prepared.reply.prepared_id, [] {
            forge::mutable_variant_object values;
            for (int index = 0; index < 64; ++index) {
               values.set("v" + std::to_string(index), std::int64_t{0});
            }
            return forge::variant_object{std::move(values)};
         }());
         successful(evaluated);
         BOOST_CHECK_EQUAL(evaluated.reply.i64, 256);
      }
   }
   BOOST_REQUIRE_MESSAGE(failed_allocation, "no allocator failure was observed inside valid CEL preparation");
}

BOOST_AUTO_TEST_CASE(expression_worker_real_evaluation_allocation_failure) {
   const auto declarations = variables(expression::type::map_int64);
   const auto values = map_activation(); // About 57 KiB logical input, below the 64 KiB profile cap.
   bool failed_evaluation = false;
   bool successful_evaluation = false;
   // Closely spaced caps expose the runtime workspace between a prepared
   // program and all decoded/CEL activation containers. A decoder-only crash is
   // deliberately not sufficient: require a response after execute() entered.
   for (std::size_t cap = 256U * 1024U; cap <= 4U * 1024U * 1024U; cap += 128U * 1024U) {
      direct_worker worker(cap);
      const auto hello = worker.command(protocol::operation::hello);
      if (hello.state != outcome::response || hello.reply.code != protocol::status::ok) {
         continue;
      }
      const auto prepared = worker.prepare("1", declarations);
      if (prepared.state != outcome::response || prepared.reply.code != protocol::status::ok) {
         continue;
      }
      const auto evaluated = worker.evaluate(prepared.reply.prepared_id, values);
      if (evaluated.state == outcome::response && allocation_failure(evaluated)) {
         failed_evaluation = true;
         BOOST_TEST_MESSAGE("actual CEL evaluation allocation refusal at cap=" << cap);
         const auto after = worker.command(protocol::operation::hello);
         successful(after);
         BOOST_CHECK_GE(after.reply.heap_failures, evaluated.reply.heap_failures);
      } else if (evaluated.state == outcome::response && evaluated.reply.code == protocol::status::ok) {
         successful(evaluated);
         BOOST_CHECK_EQUAL(evaluated.reply.i64, 1);
         successful_evaluation = true;
      } else {
         BOOST_CHECK(allocation_failure(evaluated));
      }
      if (failed_evaluation && successful_evaluation) {
         break;
      }
   }
   BOOST_REQUIRE_MESSAGE(failed_evaluation, "no recoverable CEL evaluation allocation refusal was observed");
   BOOST_REQUIRE_MESSAGE(successful_evaluation, "the same activation did not succeed with a sufficient cap");
}

BOOST_AUTO_TEST_CASE(expression_worker_fault_marker_eof_timeout_and_descriptor_lifetime) {
   {
      direct_worker warmup;
      successful(warmup.command(protocol::operation::hello));
   }
   const auto baseline = open_descriptors();
   for (int iteration = 0; iteration < 12; ++iteration) {
      direct_worker worker;
      successful(worker.command(protocol::operation::hello));
      protocol::request probe;
      probe.op = protocol::operation::memory_probe;
      probe.probe_bytes = 64U * 1024U * 1024U;
      BOOST_REQUIRE(allocation_failure(worker.exchange(std::move(probe))));
      successful(worker.command(protocol::operation::hello));
   }
   BOOST_CHECK_EQUAL(open_descriptors(), baseline);
   {
      direct_worker worker;
      successful(worker.command(protocol::operation::hello));
      worker.signal(SIGKILL);
      const auto killed = worker.command(protocol::operation::hello);
      BOOST_CHECK(killed.state == outcome::closed);
      BOOST_CHECK(!killed.exhaustion_marker);
   }
   {
      direct_worker worker;
      successful(worker.command(protocol::operation::hello));
      worker.signal(SIGSTOP);
      worker.await_stopped();
      protocol::request hello;
      hello.op = protocol::operation::hello;
      const auto started = std::chrono::steady_clock::now();
      const auto stalled = worker.exchange(std::move(hello), {}, 150ms);
      BOOST_CHECK(stalled.state == outcome::timeout);
      BOOST_CHECK(!stalled.exhaustion_marker);
      BOOST_CHECK_LT(std::chrono::steady_clock::now() - started, 2s);
   }
   BOOST_CHECK_EQUAL(open_descriptors(), baseline);
}
