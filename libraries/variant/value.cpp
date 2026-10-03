module;
#include <string.h>
#include <boost/multiprecision/cpp_int.hpp>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>
#include <vector>

#include <forge/core/macros.hpp>

module forge.variant.value;

import forge.codec.base64;
import forge.codec.hex;
import forge.core.string;
import forge.core.utf8;
import forge.variant.exceptions;

namespace forge {
namespace {

std::vector<std::uint8_t> decode_blob_base64(std::string_view value) {
   try {
      return forge::codec::base64::decode(value);
   } catch (const forge::codec::base64::exceptions::invalid_input&) {
      if (!value.ends_with('='))
         throw;

      return forge::codec::base64::decode(value.substr(0, value.size() - 1U));
   }
}

} // namespace

/**
 *  The TypeID is stored in the 'last byte' of the variant.
 */
void set_variant_type(variant* v, variant::type_id t) {
   char* data = reinterpret_cast<char*>(v);
   data[sizeof(variant) - 1] = t;
}

variant::variant() {
   set_variant_type(this, null_type);
}

variant::variant(forge::nullptr_t) {
   set_variant_type(this, null_type);
}

variant::variant(uint8_t val) {
   *reinterpret_cast<uint64_t*>(this) = val;
   set_variant_type(this, uint64_type);
}

variant::variant(int8_t val) {
   *reinterpret_cast<int64_t*>(this) = val;
   set_variant_type(this, int64_type);
}

variant::variant(uint16_t val) {
   *reinterpret_cast<uint64_t*>(this) = val;
   set_variant_type(this, uint64_type);
}

variant::variant(int16_t val) {
   *reinterpret_cast<int64_t*>(this) = val;
   set_variant_type(this, int64_type);
}

variant::variant(uint32_t val) {
   *reinterpret_cast<uint64_t*>(this) = val;
   set_variant_type(this, uint64_type);
}

variant::variant(int32_t val) {
   *reinterpret_cast<int64_t*>(this) = val;
   set_variant_type(this, int64_type);
}

variant::variant(uint64_t val) {
   *reinterpret_cast<uint64_t*>(this) = val;
   set_variant_type(this, uint64_type);
}

variant::variant(int64_t val) {
   *reinterpret_cast<int64_t*>(this) = val;
   set_variant_type(this, int64_type);
}

variant::variant(float val) {
   *reinterpret_cast<double*>(this) = val;
   set_variant_type(this, double_type);
}

variant::variant(double val) {
   *reinterpret_cast<double*>(this) = val;
   set_variant_type(this, double_type);
}

variant::variant(bool val) {
   *reinterpret_cast<bool*>(this) = val;
   set_variant_type(this, bool_type);
}

variant::variant(char* str) {
   *reinterpret_cast<std::string**>(this) = new std::string(str);
   set_variant_type(this, string_type);
}

variant::variant(const char* str) {
   *reinterpret_cast<std::string**>(this) = new std::string(str);
   set_variant_type(this, string_type);
}

variant::variant(wchar_t* str) {
   auto utf8 = std::string{};
   forge::encodeUtf8(std::wstring{str, wcslen(str)}, &utf8);
   *reinterpret_cast<std::string**>(this) = new std::string(std::move(utf8));
   set_variant_type(this, string_type);
}

variant::variant(const wchar_t* str) {
   auto utf8 = std::string{};
   forge::encodeUtf8(std::wstring{str, wcslen(str)}, &utf8);
   *reinterpret_cast<std::string**>(this) = new std::string(std::move(utf8));
   set_variant_type(this, string_type);
}

variant::variant(std::string val) {
   *reinterpret_cast<std::string**>(this) = new std::string(std::move(val));
   set_variant_type(this, string_type);
}
variant::variant(blob val) {
   *reinterpret_cast<blob**>(this) = new blob(std::move(val));
   set_variant_type(this, blob_type);
}

variant::variant(variant_object obj) {
   *reinterpret_cast<variant_object**>(this) = new variant_object(std::move(obj));
   set_variant_type(this, object_type);
}
variant::variant(mutable_variant_object obj) {
   *reinterpret_cast<variant_object**>(this) = new variant_object(std::move(obj));
   set_variant_type(this, object_type);
}

variant::variant(variants arr) {
   *reinterpret_cast<variants**>(this) = new variants(std::move(arr));
   set_variant_type(this, array_type);
}

typedef const variant_object* const_variant_object_ptr;
typedef const variants* const_variants_ptr;
typedef const blob* const_blob_ptr;
typedef const std::string* const_string_ptr;

void variant::clear() {
   switch (get_type()) {
   case object_type:
      delete *reinterpret_cast<variant_object**>(this);
      break;
   case array_type:
      delete *reinterpret_cast<variants**>(this);
      break;
   case string_type:
      delete *reinterpret_cast<std::string**>(this);
      break;
   case blob_type:
      delete *reinterpret_cast<blob**>(this);
      break;
   default:
      break;
   }
   set_variant_type(this, null_type);
}

variant::variant(const variant& v) {
   switch (v.get_type()) {
   case object_type:
      *reinterpret_cast<variant_object**>(this) =
          new variant_object(**reinterpret_cast<const const_variant_object_ptr*>(&v));
      set_variant_type(this, object_type);
      return;
   case array_type:
      *reinterpret_cast<variants**>(this) = new variants(**reinterpret_cast<const const_variants_ptr*>(&v));
      set_variant_type(this, array_type);
      return;
   case string_type:
      *reinterpret_cast<std::string**>(this) = new std::string(**reinterpret_cast<const const_string_ptr*>(&v));
      set_variant_type(this, string_type);
      return;
   case blob_type:
      *reinterpret_cast<blob**>(this) = new blob(**reinterpret_cast<const const_blob_ptr*>(&v));
      set_variant_type(this, blob_type);
      return;
   default:
      _data = v._data;
   }
}

variant::variant(variant&& v) {
   _data = v._data;
   set_variant_type(&v, null_type);
}

variant::~variant() {
   clear();
}

variant& variant::operator=(variant&& v) {
   if (this == &v)
      return *this;
   clear();
   _data = v._data;
   set_variant_type(&v, null_type);
   return *this;
}

variant& variant::operator=(const variant& v) {
   if (this == &v)
      return *this;

   clear();
   switch (v.get_type()) {
   case object_type:
      *reinterpret_cast<variant_object**>(this) =
          new variant_object((**reinterpret_cast<const const_variant_object_ptr*>(&v)));
      break;
   case array_type:
      *reinterpret_cast<variants**>(this) = new variants((**reinterpret_cast<const const_variants_ptr*>(&v)));
      break;
   case string_type:
      *reinterpret_cast<std::string**>(this) = new std::string((**reinterpret_cast<const const_string_ptr*>(&v)));
      break;
   case blob_type:
      *reinterpret_cast<blob**>(this) = new blob((**reinterpret_cast<const const_blob_ptr*>(&v)));
      break;
   default:
      _data = v._data;
   }
   set_variant_type(this, v.get_type());
   return *this;
}

void variant::visit(const visitor& v) const {
   switch (get_type()) {
   case null_type:
      v.handle();
      return;
   case int64_type:
      v.handle(*reinterpret_cast<const int64_t*>(this));
      return;
   case uint64_type:
      v.handle(*reinterpret_cast<const uint64_t*>(this));
      return;
   case double_type:
      v.handle(*reinterpret_cast<const double*>(this));
      return;
   case bool_type:
      v.handle(*reinterpret_cast<const bool*>(this));
      return;
   case string_type:
      v.handle(**reinterpret_cast<const const_string_ptr*>(this));
      return;
   case array_type:
      v.handle(**reinterpret_cast<const const_variants_ptr*>(this));
      return;
   case object_type:
      v.handle(**reinterpret_cast<const const_variant_object_ptr*>(this));
      return;
   case blob_type:
      v.handle(**reinterpret_cast<const const_blob_ptr*>(this));
      return;
   default:
      throw forge::variant_exceptions::invalid_type{"Invalid Type / Corrupted Memory"};
   }
}

variant::type_id variant::get_type() const {
   return (type_id) reinterpret_cast<const char*>(this)[sizeof(*this) - 1];
}

bool variant::is_null() const {
   return get_type() == null_type;
}

bool variant::is_string() const {
   return get_type() == string_type;
}
bool variant::is_bool() const {
   return get_type() == bool_type;
}
bool variant::is_double() const {
   return get_type() == double_type;
}
bool variant::is_uint64() const {
   return get_type() == uint64_type;
}
bool variant::is_int64() const {
   return get_type() == int64_type;
}

bool variant::is_integer() const {
   switch (get_type()) {
   case int64_type:
   case uint64_type:
   case bool_type:
      return true;
   default:
      return false;
   }
   return false;
}
bool variant::is_numeric() const {
   switch (get_type()) {
   case int64_type:
   case uint64_type:
   case double_type:
   case bool_type:
      return true;
   default:
      return false;
   }
   return false;
}

bool variant::is_object() const {
   return get_type() == object_type;
}

bool variant::is_array() const {
   return get_type() == array_type;
}
bool variant::is_blob() const {
   return get_type() == blob_type;
}

int64_t variant::as_int64() const {
   switch (get_type()) {
   case string_type:
      return to_int64(**reinterpret_cast<const const_string_ptr*>(this));
   case double_type:
      return int64_t(*reinterpret_cast<const double*>(this));
   case int64_type:
      return *reinterpret_cast<const int64_t*>(this);
   case uint64_type:
      return int64_t(*reinterpret_cast<const uint64_t*>(this));
   case bool_type:
      return *reinterpret_cast<const bool*>(this);
   case null_type:
      return 0;
   default:
      throw std::bad_cast();
   }
}

uint64_t variant::as_uint64() const {
   switch (get_type()) {
   case string_type:
      return to_uint64(**reinterpret_cast<const const_string_ptr*>(this));
   case double_type:
      return static_cast<uint64_t>(*reinterpret_cast<const double*>(this));
   case int64_type:
      return static_cast<uint64_t>(*reinterpret_cast<const int64_t*>(this));
   case uint64_type:
      return *reinterpret_cast<const uint64_t*>(this);
   case bool_type:
      return static_cast<uint64_t>(*reinterpret_cast<const bool*>(this));
   case null_type:
      return 0;
   default:
      throw std::bad_cast();
   }
}

double variant::as_double() const {
   switch (get_type()) {
   case string_type:
      return to_double(**reinterpret_cast<const const_string_ptr*>(this));
   case double_type:
      return *reinterpret_cast<const double*>(this);
   case int64_type:
      return static_cast<double>(*reinterpret_cast<const int64_t*>(this));
   case uint64_type:
      return static_cast<double>(*reinterpret_cast<const uint64_t*>(this));
   case bool_type:
      return *reinterpret_cast<const bool*>(this);
   case null_type:
      return 0;
   default:
      throw std::bad_cast();
   }
}

bool variant::as_bool() const {
   switch (get_type()) {
   case string_type: {
      const std::string& s = **reinterpret_cast<const const_string_ptr*>(this);
      if (s == "true")
         return true;
      if (s == "false")
         return false;
      throw std::bad_cast();
   }
   case double_type:
      return *reinterpret_cast<const double*>(this) != 0.0;
   case int64_type:
      return *reinterpret_cast<const int64_t*>(this) != 0;
   case uint64_type:
      return *reinterpret_cast<const uint64_t*>(this) != 0;
   case bool_type:
      return *reinterpret_cast<const bool*>(this);
   case null_type:
      return false;
   default:
      throw std::bad_cast();
   }
}

static std::string s_fc_to_string(double d) {
   // +2 is required to ensure that the double is rounded correctly when read back in.
   // http://docs.oracle.com/cd/E19957-01/806-3568/ncg_goldberg.html
   std::stringstream ss;
   ss << std::setprecision(std::numeric_limits<double>::digits10 + 2) << std::fixed << d;
   return ss.str();
}

std::string variant::as_string() const {
   switch (get_type()) {
   case string_type:
      return **reinterpret_cast<const const_string_ptr*>(this);
   case double_type:
      return s_fc_to_string(*reinterpret_cast<const double*>(this));
   case int64_type:
      return std::to_string(*reinterpret_cast<const int64_t*>(this));
   case uint64_type:
      return std::to_string(*reinterpret_cast<const uint64_t*>(this));
   case bool_type:
      return *reinterpret_cast<const bool*>(this) ? "true" : "false";
   case blob_type:
      if (get_blob().data.size())
         return forge::codec::base64::encode(std::span<const std::uint8_t>{
             reinterpret_cast<const std::uint8_t*>(get_blob().data.data()), get_blob().data.size()});
      return std::string();
   case null_type:
      return std::string();
   default:
      throw std::bad_cast();
   }
}

/// @throw if get_type() != array_type | null_type
variants& variant::get_array() {
   if (get_type() == array_type)
      return **reinterpret_cast<variants**>(this);

   throw std::bad_cast();
}
blob& variant::get_blob() {
   if (get_type() == blob_type)
      return **reinterpret_cast<blob**>(this);

   throw std::bad_cast();
}
const blob& variant::get_blob() const {
   if (get_type() == blob_type)
      return **reinterpret_cast<const const_blob_ptr*>(this);

   throw std::bad_cast();
}

blob variant::as_blob() const {
   switch (get_type()) {
   case null_type:
      return blob();
   case blob_type:
      return get_blob();
   case string_type: {
      const std::string& str = get_string();
      if (str.size() == 0)
         return blob();
      try {
         // pre-5.0 versions of variant added `=` to end of base64 encoded string in as_string() above.
         auto decoded = decode_blob_base64(str);
         return {std::move(decoded)};
      } catch (const std::exception&) {
         // unable to decode, return the raw chars
      }
      return blob({std::vector<std::uint8_t>(str.begin(), str.end())});
   }
   case object_type:
   case array_type:
      throw std::bad_cast();
   default:
      auto data = std::vector<std::uint8_t>(sizeof(_data));
      const auto representation = std::as_bytes(std::span{&_data, 1U});
      std::memcpy(data.data(), representation.data(), representation.size());
      return blob({std::move(data)});
   }
}

/// @throw if get_type() != array_type
const variants& variant::get_array() const {
   if (get_type() == array_type)
      return **reinterpret_cast<const const_variants_ptr*>(this);
   throw std::bad_cast();
}

/// @throw if get_type() != object_type | null_type
variant_object& variant::get_object() {
   if (get_type() == object_type)
      return **reinterpret_cast<variant_object**>(this);
   throw std::bad_cast();
}

const variant& variant::operator[](const char* key) const {
   return get_object()[key];
}
const variant& variant::operator[](size_t pos) const {
   return get_array()[pos];
}
/// @pre is_array()
size_t variant::size() const {
   return get_array().size();
}

size_t variant::estimated_size() const {
   switch (get_type()) {
   case null_type:
   case int64_type:
   case uint64_type:
   case double_type:
   case bool_type:
      return sizeof(*this);
   case string_type:
      return as_string().length() + sizeof(std::string) + sizeof(*this);
   case array_type: {
      const auto& arr = get_array();
      auto arr_size = arr.size();
      size_t sum = sizeof(*this) + sizeof(variants);
      for (size_t iter = 0; iter < arr_size; ++iter) {
         sum += arr[iter].estimated_size();
      }
      return sum;
   }
   case object_type:
      return get_object().estimated_size() + sizeof(*this);
   case blob_type:
      return sizeof(blob) + get_blob().data.size() + sizeof(*this);
   default:
      throw forge::variant_exceptions::invalid_type{"Invalid Type / Corrupted Memory"};
   }
}

const std::string& variant::get_string() const {
   if (get_type() == string_type)
      return **reinterpret_cast<const const_string_ptr*>(this);
   throw std::bad_cast();
}

/// @throw if get_type() != object_type
const variant_object& variant::get_object() const {
   if (get_type() == object_type)
      return **reinterpret_cast<const const_variant_object_ptr*>(this);
   throw std::bad_cast();
}

void from_variant(const variant& var, variants& vo) {
   vo = var.get_array();
}

// void from_variant( const variant& var,  variant_object& vo )
//{
//    vo  = var.get_object();
// }

void from_variant(const variant& var, variant& vo) {
   vo = var;
}

void to_variant(const uint8_t& var, variant& vo) {
   vo = uint64_t(var);
}
// TODO: warn on overflow?
void from_variant(const variant& var, uint8_t& vo) {
   vo = static_cast<uint8_t>(var.as_uint64());
}

void to_variant(const int8_t& var, variant& vo) {
   vo = int64_t(var);
}
// TODO: warn on overflow?
void from_variant(const variant& var, int8_t& vo) {
   vo = static_cast<int8_t>(var.as_int64());
}

void to_variant(const uint16_t& var, variant& vo) {
   vo = uint64_t(var);
}
// TODO: warn on overflow?
void from_variant(const variant& var, uint16_t& vo) {
   vo = static_cast<uint16_t>(var.as_uint64());
}

void to_variant(const int16_t& var, variant& vo) {
   vo = int64_t(var);
}
// TODO: warn on overflow?
void from_variant(const variant& var, int16_t& vo) {
   vo = static_cast<int16_t>(var.as_int64());
}

void to_variant(const uint32_t& var, variant& vo) {
   vo = uint64_t(var);
}
void from_variant(const variant& var, uint32_t& vo) {
   vo = static_cast<uint32_t>(var.as_uint64());
}

void to_variant(const int32_t& var, variant& vo) {
   vo = int64_t(var);
}

void from_variant(const variant& var, int32_t& vo) {
   vo = static_cast<int32_t>(var.as_int64());
}

void to_variant(const unsigned __int128& var, variant& vo) {
   vo = boost::multiprecision::uint128_t(var).str();
}

void from_variant(const variant& var, unsigned __int128& vo) {
   if (var.is_uint64()) {
      vo = var.as_uint64();
   } else if (var.is_string()) {
      vo = static_cast<unsigned __int128>(boost::multiprecision::uint128_t(var.as_string()));
   } else {
      throw std::bad_cast();
   }
}

void to_variant(const __int128& var, variant& vo) {
   vo = boost::multiprecision::int128_t(var).str();
}

void from_variant(const variant& var, __int128& vo) {
   if (var.is_int64()) {
      vo = var.as_int64();
   } else if (var.is_string()) {
      vo = static_cast<__int128>(boost::multiprecision::int128_t(var.as_string()));
   } else {
      throw std::bad_cast();
   }
}

void to_variant(const uint128& var, variant& vo) {
   vo = std::string(var);
}

void from_variant(const variant& var, uint128& vo) {
   vo = uint128(var.as_string());
}

void from_variant(const variant& var, int64_t& vo) {
   vo = var.as_int64();
}

void from_variant(const variant& var, uint64_t& vo) {
   vo = var.as_uint64();
}

void from_variant(const variant& var, bool& vo) {
   vo = var.as_bool();
}

void from_variant(const variant& var, double& vo) {
   vo = var.as_double();
}

void from_variant(const variant& var, float& vo) {
   vo = static_cast<float>(var.as_double());
}

void to_variant(const long double& value, variant& output) {
   output = static_cast<double>(value);
}

void to_variant(const std::string& s, variant& v) {
   v = variant(std::string(s));
}

void from_variant(const variant& var, std::string& vo) {
   vo = var.as_string();
}

void to_variant(const std::vector<char>& var, variant& vo) {
   if (var.size() > MAX_SIZE_OF_BYTE_ARRAYS)
      throw std::out_of_range("byte array too large");
   if (var.size())
      vo = variant(forge::codec::hex::encode(
          std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(var.data()), var.size()}));
   else
      vo = "";
}
void from_variant(const variant& var, std::vector<char>& vo) {
   const auto& str = var.get_string();
   if (str.size() > 2 * MAX_SIZE_OF_BYTE_ARRAYS)
      throw std::out_of_range("hex string too large");
   if (str.size() % 2 != 0)
      throw std::invalid_argument("the length of hex string should be even number");
   vo.resize(str.size() / 2);
   if (vo.size()) {
      const auto r = forge::codec::hex::decode(
          str, std::span<std::uint8_t>{reinterpret_cast<std::uint8_t*>(vo.data()), vo.size()});
      if (r != vo.size())
         throw forge::variant_exceptions::decode_error{"hex decode length mismatch"};
   }
}

void to_variant(const blob& b, variant& v) {
   v = variant(forge::codec::base64::encode(std::span<const std::uint8_t>{b.data.data(), b.data.size()}));
}

void from_variant(const variant& v, blob& b) {
   b.data = decode_blob_base64(v.as_string());
}

void to_variant(const UInt<8>& n, variant& v) {
   v = uint64_t(n);
}
// TODO: warn on overflow?
void from_variant(const variant& v, UInt<8>& n) {
   n = static_cast<uint8_t>(v.as_uint64());
}

void to_variant(const UInt<16>& n, variant& v) {
   v = uint64_t(n);
}
// TODO: warn on overflow?
void from_variant(const variant& v, UInt<16>& n) {
   n = static_cast<uint16_t>(v.as_uint64());
}

void to_variant(const UInt<32>& n, variant& v) {
   v = uint64_t(n);
}
// TODO: warn on overflow?
void from_variant(const variant& v, UInt<32>& n) {
   n = static_cast<uint32_t>(v.as_uint64());
}

void to_variant(const UInt<64>& n, variant& v) {
   v = uint64_t(n);
}
void from_variant(const variant& v, UInt<64>& n) {
   n = v.as_uint64();
}

#ifdef __APPLE__
void to_variant(size_t s, variant& v) {
   v = variant(uint64_t(s));
}
#elif !defined(_MSC_VER)
void to_variant(long long int s, variant& v) {
   v = variant(int64_t(s));
}
void to_variant(unsigned long long int s, variant& v) {
   v = variant(uint64_t(s));
}
#endif

bool operator==(const variant& a, const variant& b) {
   if (a.is_array() || b.is_array())
      return a.is_array() && b.is_array() && a.get_array() == b.get_array();
   if (a.is_object() || b.is_object()) {
      if (!a.is_object() || !b.is_object())
         return false;
      const auto& left = a.get_object();
      const auto& right = b.get_object();
      return left.size() == right.size() && std::is_permutation(left.begin(), left.end(), right.begin(), right.end(),
                                                                [](const auto& left_entry, const auto& right_entry) {
                                                                   return left_entry.key() == right_entry.key() &&
                                                                          left_entry.value() == right_entry.value();
                                                                });
   }
   if (a.is_blob() || b.is_blob())
      return a.is_blob() && b.is_blob() && a.get_blob().data == b.get_blob().data;
   if (a.is_string() || b.is_string())
      return a.as_string() == b.as_string();
   if (a.is_double() || b.is_double())
      return a.as_double() == b.as_double();
   if (a.is_int64() || b.is_int64())
      return a.as_int64() == b.as_int64();
   if (a.is_uint64() || b.is_uint64())
      return a.as_uint64() == b.as_uint64();
   if (a.is_bool() || b.is_bool())
      return a.is_bool() && b.is_bool() && a.as_bool() == b.as_bool();
   return a.is_null() && b.is_null();
}

bool operator!=(const variant& a, const variant& b) {
   return !(a == b);
}

bool operator!(const variant& a) {
   return !a.as_bool();
}

bool operator<(const variant& a, const variant& b) {
   if (a.is_string() || b.is_string())
      return a.as_string() < b.as_string();
   if (a.is_double() || b.is_double())
      return a.as_double() < b.as_double();
   if (a.is_int64() || b.is_int64())
      return a.as_int64() < b.as_int64();
   if (a.is_uint64() || b.is_uint64())
      return a.as_uint64() < b.as_uint64();
   throw forge::variant_exceptions::invalid_operation{"Invalid operation"};
}

bool operator>(const variant& a, const variant& b) {
   if (a.is_string() || b.is_string())
      return a.as_string() > b.as_string();
   if (a.is_double() || b.is_double())
      return a.as_double() > b.as_double();
   if (a.is_int64() || b.is_int64())
      return a.as_int64() > b.as_int64();
   if (a.is_uint64() || b.is_uint64())
      return a.as_uint64() > b.as_uint64();
   throw forge::variant_exceptions::invalid_operation{"Invalid operation"};
}

bool operator<=(const variant& a, const variant& b) {
   if (a.is_string() || b.is_string())
      return a.as_string() <= b.as_string();
   if (a.is_double() || b.is_double())
      return a.as_double() <= b.as_double();
   if (a.is_int64() || b.is_int64())
      return a.as_int64() <= b.as_int64();
   if (a.is_uint64() || b.is_uint64())
      return a.as_uint64() <= b.as_uint64();
   throw forge::variant_exceptions::invalid_operation{"Invalid operation"};
}

variant operator+(const variant& a, const variant& b) {
   if (a.is_array() && b.is_array()) {
      const variants& aa = a.get_array();
      const variants& ba = b.get_array();
      variants result;
      result.reserve(std::max(aa.size(), ba.size()));
      auto num = std::max(aa.size(), ba.size());
      for (unsigned i = 0; i < num; ++i) {
         if (aa.size() > i && ba.size() > i)
            result[i] = aa[i] + ba[i];
         else if (aa.size() > i)
            result[i] = aa[i];
         else
            result[i] = ba[i];
      }
      return result;
   }
   if (a.is_string() || b.is_string())
      return a.as_string() + b.as_string();
   if (a.is_double() || b.is_double())
      return a.as_double() + b.as_double();
   if (a.is_int64() || b.is_int64())
      return a.as_int64() + b.as_int64();
   if (a.is_uint64() || b.is_uint64())
      return a.as_uint64() + b.as_uint64();
   throw forge::variant_exceptions::invalid_operation{"invalid variant addition"};
}

variant operator-(const variant& a, const variant& b) {
   if (a.is_array() && b.is_array()) {
      const variants& aa = a.get_array();
      const variants& ba = b.get_array();
      variants result;
      result.reserve(std::max(aa.size(), ba.size()));
      auto num = std::max(aa.size(), ba.size());
      for (unsigned i = 0; i < num; --i) {
         if (aa.size() > i && ba.size() > i)
            result[i] = aa[i] - ba[i];
         else if (aa.size() > i)
            result[i] = aa[i];
         else
            result[i] = ba[i];
      }
      return result;
   }
   if (a.is_string() || b.is_string())
      return a.as_string() - b.as_string();
   if (a.is_double() || b.is_double())
      return a.as_double() - b.as_double();
   if (a.is_int64() || b.is_int64())
      return a.as_int64() - b.as_int64();
   if (a.is_uint64() || b.is_uint64())
      return a.as_uint64() - b.as_uint64();
   throw forge::variant_exceptions::invalid_operation{"invalid variant subtraction"};
}
variant operator*(const variant& a, const variant& b) {
   if (a.is_double() || b.is_double())
      return a.as_double() * b.as_double();
   if (a.is_int64() || b.is_int64())
      return a.as_int64() * b.as_int64();
   if (a.is_uint64() || b.is_uint64())
      return a.as_uint64() * b.as_uint64();
   if (a.is_array() && b.is_array()) {
      const variants& aa = a.get_array();
      const variants& ba = b.get_array();
      variants result;
      result.reserve(std::max(aa.size(), ba.size()));
      auto num = std::max(aa.size(), ba.size());
      for (unsigned i = 0; i < num; ++i) {
         if (aa.size() > i && ba.size() > i)
            result[i] = aa[i] * ba[i];
         else if (aa.size() > i)
            result[i] = aa[i];
         else
            result[i] = ba[i];
      }
      return result;
   }
   throw forge::variant_exceptions::invalid_operation{"invalid variant multiplication"};
}
variant operator/(const variant& a, const variant& b) {
   if (a.is_double() || b.is_double())
      return a.as_double() / b.as_double();
   if (a.is_int64() || b.is_int64())
      return a.as_int64() / b.as_int64();
   if (a.is_uint64() || b.is_uint64())
      return a.as_uint64() / b.as_uint64();
   if (a.is_array() && b.is_array()) {
      const variants& aa = a.get_array();
      const variants& ba = b.get_array();
      variants result;
      result.reserve(std::max(aa.size(), ba.size()));
      auto num = std::max(aa.size(), ba.size());
      for (unsigned i = 0; i < num; ++i) {
         if (aa.size() > i && ba.size() > i)
            result[i] = aa[i] / ba[i];
         else if (aa.size() > i)
            result[i] = aa[i];
         else
            result[i] = ba[i];
      }
      return result;
   }
   throw forge::variant_exceptions::invalid_operation{"invalid variant division"};
}
} // namespace forge
