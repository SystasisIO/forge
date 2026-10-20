module;

#include <cstdint>
#include <vector>

export module forge.swarm.protocol.metadata_assembler;

export import forge.swarm.protocol.metadata;

export namespace forge::swarm::protocol {

class metadata_assembler {
 public:
   explicit metadata_assembler(core::swarm_id expected);
   void append(const metadata_chunk&);
   [[nodiscard]] bool complete() const noexcept;
   [[nodiscard]] std::uint32_t received_size() const noexcept;
   [[nodiscard]] metadata finish() &&;

 private:
   core::swarm_id _expected;
   std::vector<std::uint8_t> _bytes;
   std::uint32_t _total = 0;
   bool _finished = false;
};

} // namespace forge::swarm::protocol
