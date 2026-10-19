module;

#include <boost/describe.hpp>
#include <string>
#include <vector>

export module fixture.grant;

export import forge.auth.attestation.document;

export namespace fixture {

struct byte_range {
   std::string offset;
   std::string length;

   bool operator==(const byte_range&) const = default;
};

struct read_claims {
   std::string object;
   std::string generation;
   std::vector<byte_range> ranges;
   std::string note;

   bool operator==(const read_claims&) const = default;
};

using grant = forge::auth::attestation::document<read_claims>;

BOOST_DESCRIBE_STRUCT(byte_range, (), (offset, length))
BOOST_DESCRIBE_STRUCT(read_claims, (), (object, generation, ranges, note))

} // namespace fixture
