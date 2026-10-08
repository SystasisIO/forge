#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

import forge.asio.runtime;
import forge.codec.json;
import forge.crypto.digest.sha256;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.pubsub;
import forge.variant.value;
import forge.variant.containers;

#include "forge_pubsub_fixture.hxx"

namespace forge::test::libp2p_interop {

// Synthetic fixture units: no host, transport, router decision or interop evidence is created.
void forge_pubsub_fixture::self_test() {
   auto check = [](bool okay, std::string_view reason) {
      if (!okay) { throw std::runtime_error{"PubSub fixture unit: " + std::string{reason}}; }
   };
   auto rejects = [&](auto work, std::string_view reason) {
      try { work(); }
      catch (const std::exception& error) {
         check(std::string_view{error.what()}.find(reason) != std::string_view::npos, "wrong rejection reason");
         return;
      }
      check(false, "missing rejection: " + std::string{reason});
   };
   for (const auto transport : {"tcp", "tcp-pnet-noise", "quic"}) {
      auto options = forge::net::p2p::node::options{};
      options.stream_security = forge::net::p2p::node::stream_security::tls;
      configure_stream_security(options, transport);
      const auto expected = std::string_view{transport} == "quic" ? forge::net::p2p::node::stream_security::tls
                                                               : forge::net::p2p::node::stream_security::noise;
      check(options.stream_security == expected, "fixture stream security differs from its forced profile");
   }
   const auto local_peer = forge::net::p2p::make_peer_id({
       .type = forge::net::p2p::public_key::type::ed25519, .data = std::vector<std::uint8_t>(32, 1)});
   const auto other_peer = forge::net::p2p::make_peer_id({
       .type = forge::net::p2p::public_key::type::ed25519, .data = std::vector<std::uint8_t>(32, 2)});
   const auto prepare = forge::variant{forge::mutable_variant_object{}("sequence", 1u)("kind", "prepare_shutdown")
       ("actor", "victim")("case_token", std::string(32, 'a'))("local_peer_id", local_peer.to_string())};
   for (const auto state : {"active_error", "overflow", "foreign_actor", "foreign_token", "foreign_identity"}) {
      auto fixture = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
      fixture._peer = local_peer.to_string();
      auto input = forge::mutable_variant_object{prepare.get_object()};
      if (state == std::string_view{"active_error"}) { fixture._capture_error = "active native failure"; }
      if (state == std::string_view{"overflow"}) { fixture._overflow = true; }
      if (state == std::string_view{"foreign_actor"}) { input("actor", "sink"); }
      if (state == std::string_view{"foreign_token"}) { input("case_token", std::string(32, 'b')); }
      if (state == std::string_view{"foreign_identity"}) { input("local_peer_id", other_peer.to_string()); }
      rejects([&] { fixture.prepare_shutdown(forge::variant{input}, 1); }, "prepare_shutdown");
      check(!fixture._prepared && fixture._events.empty(), "failed prepare emitted acknowledgement");
      if (state == std::string_view{"active_error"}) {
         check(fixture._capture_error == "active native failure", "late prepare cleared active failure");
      }
   }
   auto prepared = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
   prepared._peer = local_peer.to_string();
   prepared.prepare_shutdown(prepare, 1);
   check(prepared._prepared && prepared._events.size() == 1 &&
       prepared._events.front()["local_peer_id"].get_string() == local_peer.to_string() &&
       prepared._events.front()["admission_closed"].as_bool(), "prepare acknowledgement lost native identity/admission");
   rejects([&] { prepared.prepare_shutdown(prepare, 1); }, "prepare_shutdown");
   {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      rejects([&] { prepared.command(prepare, 2, runtime); }, "admission closed");
      runtime.stop();
   }
   prepared._capture_error = "real teardown error";
   check(prepared.result(true, true, {})["error"].get_string() == "real teardown error", "prepare masked teardown failure");
   for (const auto text : {"/ip4/127.0.0.1/tcp/1234", "/ip4/127.0.0.1/udp/1234/quic-v1"}) {
      const auto native = forge::net::p2p::parse_endpoint(text);
      auto canonical = native;
      canonical.peer = local_peer;
      const auto address = ready_address(native, local_peer);
      check(!native.peer && address == canonical.to_string(), "ready address changed its typed native listener");
      const auto decoded = forge::net::p2p::parse_endpoint(address);
      check(decoded.peer == local_peer && !decoded.relayed, "ready address lost its native peer binding");
      check(ready_address(canonical, local_peer) == address, "canonical listener gained a duplicate peer suffix");
      canonical.peer = other_peer;
      rejects([&] { static_cast<void>(ready_address(canonical, local_peer)); }, "peer mismatch");
      canonical.peer = local_peer;
      canonical.relayed = forge::net::p2p::endpoint::circuit{.target = other_peer};
      rejects([&] { static_cast<void>(ready_address(canonical, local_peer)); }, "direct native listener");
   }
   struct scratch {
      std::filesystem::path path;
      ~scratch() { auto error = std::error_code{}; std::filesystem::remove_all(path, error); }
   };
   const auto root = std::filesystem::temp_directory_path() / ("forge-pubsub-fixture-unit-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
   check(std::filesystem::create_directory(root), "scratch directory is not fresh");
   const auto cleanup = scratch{root};
   auto write = [&](const auto& path, std::string_view bytes, bool append = false) {
      auto output = std::ofstream{path, std::ios::binary | (append ? std::ios::app : std::ios::trunc)};
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      output.close();
      check(static_cast<bool>(output), "cannot write scratch file");
   };
   auto line = [](std::uint64_t sequence) {
      return "{\"sequence\":" + std::to_string(sequence) + ",\"kind\":\"sample\",\"label\":\"before\"}\n";
   };
   auto calls = std::uint64_t{};
   auto apply = [&](const forge::variant& command, std::uint64_t sequence) {
      check(command["sequence"].as_uint64() == sequence && sequence == calls + 1, "noncontiguous application");
      ++calls;
   };
   const auto control = root / "control";
   auto reader = control_reader{control};
   reader.poll(control, apply);
   const auto one = line(1);
   write(control, std::string_view{one}.substr(0, one.size() - 1));
   reader.poll(control, apply);
   check(calls == 0, "partial line was applied");
   rejects([&] { reader.poll(control, apply, true); }, "incomplete");
   write(control, "\n", true);
   reader.poll(control, apply);
   reader.poll(control, apply);
   reader.poll(control, apply, true);
   check(calls == 1, "newline completion replayed or lost");
   auto rewritten = one;
   rewritten.replace(rewritten.find("before"), 6, "after!");
   write(control, rewritten + line(2));
   rejects([&] { reader.poll(control, apply); }, "prefix was rewritten");
   check(calls == 1, "command applied after prefix rewrite");
   write(control, one);
   write(control, line(2), true);
   rejects([&] { reader.poll(control, apply, true); }, "unprocessed");
   write(control, std::string_view{one}.substr(0, one.size() - 1));
   rejects([&] { reader.poll(control, apply); }, "truncated");
   std::filesystem::remove(control);

   const auto prepare_line = "{\"sequence\":1,\"kind\":\"prepare_shutdown\"}\n";
   for (const auto& tail : {line(2), std::string{"{"}}) {
      write(control, prepare_line + tail);
      auto queued = control_reader{control};
      rejects([&] { queued.poll(control, [&](const auto&, auto) { check(false, "prepare with pending command was applied"); }); }, "queued/incomplete");
   }
   write(control, prepare_line);
   auto closed_admission = control_reader{control};
   auto preparations = 0u;
   closed_admission.poll(control, [&](const auto&, auto) { ++preparations; });
   write(control, "{", true);
   rejects([&] { closed_admission.poll(control, [&](const auto&, auto) { ++preparations; }); }, "admission closed");
   check(preparations == 1, "control admitted commands after prepare");
   std::filesystem::remove(control);
   rejects([&] { reader.poll(control, apply); }, "disappeared");

   calls = 0;
   auto limits = control_reader{control};
   auto exact_line = one.substr(0, one.size() - 1);
   exact_line.resize(16 * 1024, ' ');
   write(control, exact_line);
   limits.poll(control, apply);
   check(calls == 0, "exact-bound partial line was applied");
   write(control, "\n", true);
   limits.poll(control, apply);
   for (auto sequence = 2u; sequence <= 64; ++sequence) { write(control, line(sequence), true); }
   limits.poll(control, apply);
   limits.poll(control, apply);
   check(calls == 64, "64 complete commands were not applied exactly once");
   write(control, line(65), true);
   rejects([&] { limits.poll(control, apply); }, "count bound");
   check(calls == 64, "65th command was applied");
   auto oversized = control_reader{control};
   write(control, std::string(16 * 1024 + 1, ' '));
   rejects([&] { oversized.poll(control, apply); }, "line bound");
   write(control, std::string(64 * (16 * 1024 + 1) + 1, ' '));
   rejects([&] { oversized.poll(control, apply); }, "over bound");
   std::filesystem::remove(control);

   auto paths = [&] {
      auto args = arguments{};
      for (const auto flag : {"ready-file", "result-file", "control-file", "stop-file", "store-dir"}) {
         args[flag] = (root / flag).string();
      }
      return args;
   };
   auto fresh = paths();
   canonical_paths(fresh, false);
   check(std::filesystem::path{fresh.at("ready-file")}.is_absolute(), "noncanonical output path");
   auto aliased = paths();
   aliased["control-file"] = (root / "." / "ready-file").string();
   rejects([&] { canonical_paths(aliased, false); }, "distinct");
   aliased = paths();
   aliased["control-file"] = aliased.at("ready-file") + ".tmp";
   rejects([&] { canonical_paths(aliased, false); }, "temporary paths");
   aliased = paths();
   aliased["store-dir"] = aliased.at("result-file");
   rejects([&] { canonical_paths(aliased, false); }, "distinct");
   for (const auto flag : {"ready-file", "result-file", "stop-file"}) {
      write(fresh.at(flag), "");
      rejects([&] { auto args = paths(); canonical_paths(args, false); }, "must be fresh");
      std::filesystem::remove(fresh.at(flag));
   }
   write(fresh.at("control-file"), "");
   canonical_paths(fresh, false);
   write(fresh.at("control-file"), one);
   rejects([&] { canonical_paths(fresh, false); }, "fresh empty regular file");
   std::filesystem::remove(fresh.at("control-file"));
   for (const auto flag : {"ready-file", "result-file"}) {
      const auto temporary = fresh.at(flag) + ".tmp";
      write(temporary, "");
      rejects([&] { auto args = paths(); canonical_paths(args, false); }, "temporary paths");
      rejects([&] { write_atomic(fresh.at(flag), forge::variant{}); }, "temporary file");
      std::filesystem::remove(temporary);
   }
   auto private_paths = paths();
   private_paths["pnet-key-file"] = (root / "key").string();
   write(private_paths.at("pnet-key-file"), std::string(1025, 'x'));
   rejects([&] { canonical_paths(private_paths, true); }, "bounded");
   private_paths["pnet-key-file"] = private_paths.at("stop-file");
   rejects([&] { canonical_paths(private_paths, true); }, "distinct");

   auto observer = forge_pubsub_fixture{std::string(32, 'a'), "victim"};
   const auto peer = forge::net::p2p::peer_id{.value = "synthetic-owner"};
   observer._connections.emplace(7, native_owner{.peer = peer, .remote_address = "synthetic-endpoint",
       .transport = "synthetic-transport", .security = "synthetic-security", .muxer = "synthetic-muxer"});
   check(!observer._node && observer.cached_owner_locked(7, peer), "cached owner requires active session");
   check(!observer.cached_owner_locked(8, peer), "unknown connection became authenticated");
   rejects([&] { static_cast<void>(observer.cached_owner_locked(7, {.value = "other-owner"})); }, "peer mismatch");
   check(observer._events.empty(), "synthetic owner emitted native proof");
   observer._peer.assign(512, '"');
   for (auto count = 0u; count < 300 && !observer._overflow; ++count) {
      observer.record("synthetic", "fixture.unit", forge::mutable_variant_object{}("payload", std::string(63 * 1024, 'x')));
   }
   check(observer._overflow && observer._events.size() < 2048, "trace byte overflow was not sticky");
   const auto events = observer._events.size();
   observer.record("synthetic", "fixture.unit", {});
   check(observer._events.size() == events, "overflow admitted another event");
   const auto final_path = root / "overflow.json";
   write_atomic(final_path, observer.result(true, true, std::string(1024, '"')));
   const auto size = std::filesystem::file_size(final_path);
   check(size <= 16 * 1024 * 1024 && size > 15 * 1024 * 1024, "final JSON bound/reserve not exercised");
   const auto decoded = forge::codec::json::load_value(final_path);
   check(decoded.ok() && decoded.value["finalized"].as_bool() && decoded.value["joined"].as_bool() &&
       decoded.value["overflow"].as_bool() && decoded.value["error"].get_string().size() == 512 &&
       decoded.value["events"].get_array().size() == events, "overflow final JSON lost terminal evidence");
   auto count_bound = forge_pubsub_fixture{std::string(32, 'b'), "victim"};
   for (auto count = 0u; count <= 2048; ++count) { count_bound.record("synthetic", "fixture.unit", {}); }
   check(count_bound._overflow && count_bound._events.size() == 2048, "event count bound failed");
   auto event_bound = forge_pubsub_fixture{std::string(32, 'c'), "victim"};
   event_bound.record("synthetic", "fixture.unit", forge::mutable_variant_object{}("payload", std::string(64 * 1024, 'x')));
   check(event_bound._overflow && !event_bound._capture_error.empty() && event_bound._events.empty(), "event byte bound failed");
}

} // namespace forge::test::libp2p_interop
