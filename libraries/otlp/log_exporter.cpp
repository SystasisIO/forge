module;

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

module forge.otlp.log_exporter;

import forge.asio.notification;
import forge.net.http.base_url;
import forge.net.http.client;
import forge.net.http.types;

#include "details/log_exporter_impl.hxx"

namespace forge::otlp {

log_exporter::log_exporter(forge::asio::runtime& runtime, log_exporter_options options)
    : impl_(std::make_shared<impl>(runtime, std::move(options))) {}

log_exporter::~log_exporter() {
   impl_->close_without_flush();
}

bool log_exporter::enqueue(const forge::log_record& record) {
   return impl_->enqueue(record);
}

exporter_metrics log_exporter::metrics() const {
   return impl_->metrics();
}

boost::asio::awaitable<export_result> log_exporter::async_export(std::vector<forge::log_record> records) {
   auto owner = impl_;
   return boost::asio::co_spawn(owner->strand, impl::export_owned(owner, std::move(records)),
                                 boost::asio::use_awaitable);
}

boost::asio::awaitable<void> log_exporter::async_flush() {
   auto owner = impl_;
   return boost::asio::co_spawn(owner->strand, impl::flush_owned(owner), boost::asio::use_awaitable);
}

boost::asio::awaitable<void> log_exporter::async_shutdown() {
   auto owner = impl_;
   return boost::asio::co_spawn(owner->strand, impl::shutdown_owned(owner), boost::asio::use_awaitable);
}

} // namespace forge::otlp
