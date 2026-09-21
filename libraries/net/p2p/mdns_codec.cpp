#include "details/mdns_codec.hxx"

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

import forge.net.p2p.exceptions;

namespace forge::net::p2p::detail::mdns_codec {
namespace {

[[noreturn]] void fail(std::string_view reason) {
   FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::codec_error, reason);
}

void require(bool condition, std::string_view reason) {
   if (!condition) {
      fail(reason);
   }
}

void validate_limits(const limits& bounds) {
   require(bounds.max_packet_size >= 12 && bounds.max_packet_size <= 65535,
           "mDNS packet limit must be in [12, 65535]");
   require(bounds.max_pointer_depth <= 128, "mDNS pointer depth limit exceeds 128");
}

void require_bytes(std::size_t offset, std::size_t count, std::size_t end) {
   require(offset <= end && count <= end - offset, "truncated mDNS field");
}

std::uint16_t read_u16(std::span<const std::uint8_t> packet, std::size_t& offset, std::size_t end) {
   require_bytes(offset, 2, end);
   const auto result = static_cast<std::uint16_t>(
       (static_cast<std::uint16_t>(packet[offset]) << 8U) | packet[offset + 1]);
   offset += 2;
   return result;
}

std::uint32_t read_u32(std::span<const std::uint8_t> packet, std::size_t& offset, std::size_t end) {
   const auto high = read_u16(packet, offset, end);
   const auto low = read_u16(packet, offset, end);
   return (static_cast<std::uint32_t>(high) << 16U) | low;
}

void read_name(std::span<const std::uint8_t> packet, std::size_t& offset, std::size_t end,
               const limits& bounds, name* output) {
   auto cursor = offset;
   auto jumped = false;
   auto expanded = std::size_t{1}; // Root terminator is part of the 255-byte limit.
   auto depth = std::size_t{};
   auto targets = std::array<std::size_t, 128>{};
   for (;;) {
      const auto current_end = jumped ? packet.size() : end;
      require_bytes(cursor, 1, current_end);
      const auto length = packet[cursor];
      if ((length & 0xc0U) == 0xc0U) {
         require_bytes(cursor, 2, current_end);
         require(depth < bounds.max_pointer_depth, "mDNS compression depth exceeded");
         const auto target = static_cast<std::size_t>(((length & 0x3fU) << 8U) | packet[cursor + 1]);
         require(target >= 12 && target < packet.size(), "mDNS compression pointer outside name data");
         require(std::find(targets.begin(), targets.begin() + static_cast<std::ptrdiff_t>(depth), target) ==
                     targets.begin() + static_cast<std::ptrdiff_t>(depth),
                 "mDNS compression pointer cycle");
         require(target < cursor, "mDNS compression pointer must reference prior name data");
         targets[depth++] = target;
         if (!jumped) {
            offset = cursor + 2;
         }
         jumped = true;
         cursor = target;
         continue;
      }
      require((length & 0xc0U) == 0, "invalid mDNS label tag");
      ++cursor;
      if (length == 0) {
         if (!jumped) {
            offset = cursor;
         }
         return;
      }
      require(length <= 63 && length + 1U <= 255U - expanded, "mDNS expanded name exceeds 255 bytes");
      require_bytes(cursor, length, current_end);
      expanded += length + 1U;
      if (output) {
         output->emplace_back(reinterpret_cast<const char*>(packet.data() + cursor), length);
      }
      cursor += length;
   }
}

void consume_budget(std::size_t& used, std::size_t amount, std::size_t limit, std::string_view reason) {
   require(used <= limit && amount <= limit - used, reason);
   used += amount;
}

void check_counts(const std::array<std::size_t, 4>& counts, const limits& bounds) {
   require(counts[0] <= 65535 && counts[0] <= bounds.max_questions, "mDNS question count exceeds limit");
   auto total = std::size_t{};
   for (auto index = std::size_t{1}; index < counts.size(); ++index) {
      require(counts[index] <= 65535 && counts[index] <= bounds.max_records_per_section,
              "mDNS section count exceeds limit");
      consume_budget(total, counts[index], bounds.max_records, "mDNS record count exceeds limit");
   }
}

void read_record(std::span<const std::uint8_t> packet, std::size_t& offset, const limits& bounds,
                 std::size_t& txt_count, std::size_t& txt_bytes, record* output) {
   read_name(packet, offset, packet.size(), bounds, output ? &output->owner : nullptr);
   const auto type = read_u16(packet, offset, packet.size());
   const auto class_code = read_u16(packet, offset, packet.size());
   const auto ttl = read_u32(packet, offset, packet.size());
   const auto size = read_u16(packet, offset, packet.size());
   require(size <= bounds.max_rdata_size, "mDNS RDATA size exceeds limit");
   require_bytes(offset, size, packet.size());
   const auto end = offset + size;
   if (output) {
      output->type = type;
      output->class_code = class_code;
      output->ttl = ttl;
   }
   switch (type) {
   case type_ptr:
      if (output) {
         output->data = ptr{};
      }
      read_name(packet, offset, end, bounds, output ? &std::get<ptr>(output->data).target : nullptr);
      break;
   case type_srv: {
      const auto priority = read_u16(packet, offset, end);
      const auto weight = read_u16(packet, offset, end);
      const auto port = read_u16(packet, offset, end);
      if (output) {
         output->data = srv{.priority = priority, .weight = weight, .port = port};
      }
      read_name(packet, offset, end, bounds, output ? &std::get<srv>(output->data).target : nullptr);
      break;
   }
   case type_txt: {
      if (output) {
         output->data = txt{};
      }
      auto count = std::size_t{};
      auto payload = std::size_t{};
      while (offset < end) {
         const auto length = packet[offset++];
         require_bytes(offset, length, end);
         consume_budget(count, 1, bounds.max_txt_attributes_per_record, "mDNS TXT attribute count exceeds limit");
         consume_budget(txt_count, 1, bounds.max_txt_attributes, "mDNS packet TXT count exceeds limit");
         consume_budget(payload, length, bounds.max_txt_bytes_per_record, "mDNS TXT payload exceeds limit");
         consume_budget(txt_bytes, length, bounds.max_txt_bytes, "mDNS packet TXT payload exceeds limit");
         if (output) {
            std::get<txt>(output->data).attributes.emplace_back(packet.begin() + static_cast<std::ptrdiff_t>(offset),
                                                              packet.begin() + static_cast<std::ptrdiff_t>(offset + length));
         }
         offset += length;
      }
      break;
   }
   case type_a:
      require(size == 4, "mDNS A RDATA must have four bytes");
      if (output) {
         auto address = a{};
         std::copy_n(packet.begin() + static_cast<std::ptrdiff_t>(offset), 4, address.address.begin());
         output->data = address;
      }
      offset = end;
      break;
   case type_aaaa:
      require(size == 16, "mDNS AAAA RDATA must have sixteen bytes");
      if (output) {
         auto address = aaaa{};
         std::copy_n(packet.begin() + static_cast<std::ptrdiff_t>(offset), 16, address.address.begin());
         output->data = address;
      }
      offset = end;
      break;
   default:
      if (output) {
         output->data = unknown{.wire_size = size};
      }
      offset = end;
      break;
   }
   require(offset == end, "mDNS RDATA length mismatch");
}

void read_message(std::span<const std::uint8_t> packet, const limits& bounds, message* output) {
   auto offset = std::size_t{};
   const auto id = read_u16(packet, offset, packet.size());
   const auto flags = read_u16(packet, offset, packet.size());
   auto counts = std::array<std::size_t, 4>{};
   for (auto& count : counts) {
      count = read_u16(packet, offset, packet.size());
   }
   check_counts(counts, bounds);
   // Even root questions/records occupy at least 5/11 bytes on the wire.
   require(counts[0] * 5 + (counts[1] + counts[2] + counts[3]) * 11 <= packet.size() - offset,
           "mDNS counts cannot fit in the packet");
   if (output) {
      output->head = {.id = id, .flags = flags};
      output->questions.reserve(counts[0]);
   }
   for (auto index = std::size_t{}; index < counts[0]; ++index) {
      auto item = question{};
      read_name(packet, offset, packet.size(), bounds, output ? &item.owner : nullptr);
      const auto type = read_u16(packet, offset, packet.size());
      const auto class_code = read_u16(packet, offset, packet.size());
      if (output) {
         item.type = type;
         item.class_code = class_code;
         output->questions.push_back(std::move(item));
      }
   }
   auto txt_count = std::size_t{};
   auto txt_bytes = std::size_t{};
   auto sections = std::array<std::vector<record>*, 3>{
       output ? &output->answers : nullptr, output ? &output->authorities : nullptr,
       output ? &output->additionals : nullptr};
   for (auto section = std::size_t{}; section < sections.size(); ++section) {
      if (sections[section]) {
         sections[section]->reserve(counts[section + 1]);
      }
      for (auto index = std::size_t{}; index < counts[section + 1]; ++index) {
         auto item = record{};
         read_record(packet, offset, bounds, txt_count, txt_bytes, output ? &item : nullptr);
         if (sections[section]) {
            sections[section]->push_back(std::move(item));
         }
      }
   }
   require(offset == packet.size(), "mDNS packet has trailing bytes");
}

void emit_byte(bytes* output, std::size_t& size, std::uint8_t value, const limits& bounds) {
   consume_budget(size, 1, bounds.max_packet_size, "encoded mDNS packet exceeds limit");
   if (output) {
      output->push_back(value);
   }
}

void emit_u16(bytes* output, std::size_t& size, std::uint16_t value, const limits& bounds) {
   emit_byte(output, size, static_cast<std::uint8_t>(value >> 8U), bounds);
   emit_byte(output, size, static_cast<std::uint8_t>(value), bounds);
}

void emit_bytes(bytes* output, std::size_t& size, std::span<const std::uint8_t> value, const limits& bounds) {
   consume_budget(size, value.size(), bounds.max_packet_size, "encoded mDNS packet exceeds limit");
   if (output) {
      output->insert(output->end(), value.begin(), value.end());
   }
}

void emit_name(bytes* output, std::size_t& size, const name& value, const limits& bounds) {
   auto expanded = std::size_t{1};
   for (const auto& label : value) {
      require(!label.empty() && label.size() <= 63, "mDNS label length must be in [1, 63]");
      consume_budget(expanded, label.size() + 1, 255, "mDNS name exceeds 255 bytes");
      emit_byte(output, size, static_cast<std::uint8_t>(label.size()), bounds);
      emit_bytes(output, size,
                 {reinterpret_cast<const std::uint8_t*>(label.data()), label.size()}, bounds);
   }
   emit_byte(output, size, 0, bounds);
}

void emit_record(bytes* output, std::size_t& size, const record& item, const limits& bounds,
                 std::size_t& txt_count, std::size_t& txt_bytes) {
   emit_name(output, size, item.owner, bounds);
   emit_u16(output, size, item.type, bounds);
   emit_u16(output, size, item.class_code, bounds);
   emit_u16(output, size, static_cast<std::uint16_t>(item.ttl >> 16U), bounds);
   emit_u16(output, size, static_cast<std::uint16_t>(item.ttl), bounds);
   const auto length_offset = size;
   emit_u16(output, size, 0, bounds);
   const auto start = size;
   if (const auto* value = std::get_if<ptr>(&item.data)) {
      require(item.type == type_ptr, "mDNS PTR type mismatch");
      emit_name(output, size, value->target, bounds);
   } else if (const auto* value = std::get_if<srv>(&item.data)) {
      require(item.type == type_srv, "mDNS SRV type mismatch");
      emit_u16(output, size, value->priority, bounds);
      emit_u16(output, size, value->weight, bounds);
      emit_u16(output, size, value->port, bounds);
      emit_name(output, size, value->target, bounds);
   } else if (const auto* value = std::get_if<txt>(&item.data)) {
      require(item.type == type_txt, "mDNS TXT type mismatch");
      require(value->attributes.size() <= bounds.max_txt_attributes_per_record,
              "mDNS TXT attribute count exceeds limit");
      consume_budget(txt_count, value->attributes.size(), bounds.max_txt_attributes,
                     "mDNS packet TXT count exceeds limit");
      auto payload = std::size_t{};
      for (const auto& attribute : value->attributes) {
         require(attribute.size() <= 255, "mDNS TXT attribute exceeds 255 bytes");
         consume_budget(payload, attribute.size(), bounds.max_txt_bytes_per_record, "mDNS TXT payload exceeds limit");
         consume_budget(txt_bytes, attribute.size(), bounds.max_txt_bytes, "mDNS packet TXT payload exceeds limit");
         emit_byte(output, size, static_cast<std::uint8_t>(attribute.size()), bounds);
         emit_bytes(output, size, attribute, bounds);
      }
   } else if (const auto* value = std::get_if<a>(&item.data)) {
      require(item.type == type_a, "mDNS A type mismatch");
      emit_bytes(output, size, value->address, bounds);
   } else if (const auto* value = std::get_if<aaaa>(&item.data)) {
      require(item.type == type_aaaa, "mDNS AAAA type mismatch");
      emit_bytes(output, size, value->address, bounds);
   } else {
      fail("cannot encode skipped unknown mDNS RDATA");
   }
   const auto length = size - start;
   require(length <= 65535 && length <= bounds.max_rdata_size, "encoded mDNS RDATA exceeds limit");
   if (output) {
      (*output)[length_offset] = static_cast<std::uint8_t>(length >> 8U);
      (*output)[length_offset + 1] = static_cast<std::uint8_t>(length);
   }
}

std::size_t emit_message(bytes* output, const message& value, const limits& bounds) {
   const auto counts = std::array<std::size_t, 4>{
       value.questions.size(), value.answers.size(), value.authorities.size(), value.additionals.size()};
   check_counts(counts, bounds);
   auto size = std::size_t{};
   emit_u16(output, size, value.head.id, bounds);
   emit_u16(output, size, value.head.flags, bounds);
   for (const auto count : counts) {
      emit_u16(output, size, static_cast<std::uint16_t>(count), bounds);
   }
   for (const auto& item : value.questions) {
      emit_name(output, size, item.owner, bounds);
      emit_u16(output, size, item.type, bounds);
      emit_u16(output, size, item.class_code, bounds);
   }
   auto txt_count = std::size_t{};
   auto txt_bytes = std::size_t{};
   for (const auto* section : {&value.answers, &value.authorities, &value.additionals}) {
      for (const auto& item : *section) {
         emit_record(output, size, item, bounds, txt_count, txt_bytes);
      }
   }
   return size;
}

} // namespace

message decode(std::span<const std::uint8_t> packet, const limits& bounds) {
   validate_limits(bounds);
   require(packet.size() >= 12 && packet.size() <= bounds.max_packet_size, "mDNS packet size outside limits");
   read_message(packet, bounds, nullptr);
   auto result = message{};
   read_message(packet, bounds, &result);
   return result;
}

bytes encode(const message& value, const limits& bounds) {
   validate_limits(bounds);
   const auto size = emit_message(nullptr, value, bounds);
   auto result = bytes{};
   result.reserve(size);
   static_cast<void>(emit_message(&result, value, bounds));
   return result;
}

} // namespace forge::net::p2p::detail::mdns_codec
