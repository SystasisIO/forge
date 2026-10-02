module;
#include <forge/core/macros.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <boost/describe.hpp>
#include <boost/multi_index_container_fwd.hpp>
#include <boost/multiprecision/cpp_int.hpp>

export module forge.variant.value;

import forge.core.uint128;
import forge.core.utility;

export namespace forge {
class variant;
class variant_object;
class mutable_variant_object;

struct blob {
   std::vector<std::uint8_t> data;
};
using variants = std::vector<variant>;
using ovariant = std::optional<variant>;

template <size_t Size>
using UInt = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
    Size, Size, boost::multiprecision::unsigned_magnitude, boost::multiprecision::unchecked, void>>;
template <size_t Size>
using Int = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
    Size, Size, boost::multiprecision::signed_magnitude, boost::multiprecision::unchecked, void>>;

void to_variant(const blob& var, variant& vo);
void from_variant(const variant& var, blob& vo);
void to_variant(const uint8_t& var, variant& vo);
void from_variant(const variant& var, uint8_t& vo);
void to_variant(const int8_t& var, variant& vo);
void from_variant(const variant& var, int8_t& vo);
void to_variant(const uint16_t& var, variant& vo);
void from_variant(const variant& var, uint16_t& vo);
void to_variant(const int16_t& var, variant& vo);
void from_variant(const variant& var, int16_t& vo);
void to_variant(const uint32_t& var, variant& vo);
void from_variant(const variant& var, uint32_t& vo);
void to_variant(const int32_t& var, variant& vo);
void from_variant(const variant& var, int32_t& vo);
void to_variant(const unsigned __int128& var, variant& vo);
void from_variant(const variant& var, unsigned __int128& vo);
void to_variant(const __int128& var, variant& vo);
void from_variant(const variant& var, __int128& vo);
void to_variant(const long double& var, variant& vo);
void to_variant(const uint128& var, variant& vo);
void from_variant(const variant& var, uint128& vo);
void to_variant(const variant_object& var, variant& vo);
void from_variant(const variant& var, variant_object& vo);
void to_variant(const mutable_variant_object& var, variant& vo);
void from_variant(const variant& var, mutable_variant_object& vo);
void to_variant(const std::vector<char>& var, variant& vo);
void from_variant(const variant& var, std::vector<char>& vo);
void to_variant(const std::string& s, variant& v);
void from_variant(const variant& var, std::string& vo);
void from_variant(const variant& var, variants& vo);
void from_variant(const variant& var, variant& vo);
void from_variant(const variant& var, int64_t& vo);
void from_variant(const variant& var, uint64_t& vo);
void from_variant(const variant& var, bool& vo);
void from_variant(const variant& var, double& vo);
void from_variant(const variant& var, float& vo);
void to_variant(const UInt<8>& n, variant& v);
void from_variant(const variant& v, UInt<8>& n);
void to_variant(const UInt<16>& n, variant& v);
void from_variant(const variant& v, UInt<16>& n);
void to_variant(const UInt<32>& n, variant& v);
void from_variant(const variant& v, UInt<32>& n);
void to_variant(const UInt<64>& n, variant& v);
void from_variant(const variant& v, UInt<64>& n);
#ifdef __APPLE__
void to_variant(size_t s, variant& v);
#elif !defined(_MSC_VER)
void to_variant(long long int s, variant& v);
void to_variant(unsigned long long int s, variant& v);
#endif

class variant {
 public:
   enum type_id {
      null_type = 0,
      int64_type = 1,
      uint64_type = 2,
      double_type = 3,
      bool_type = 4,
      string_type = 5,
      array_type = 6,
      object_type = 7,
      blob_type = 8
   };
   BOOST_DESCRIBE_NESTED_ENUM(type_id, null_type, int64_type, uint64_type, double_type, bool_type, string_type,
                              array_type, object_type, blob_type)

   variant();
   variant(nullptr_t);
   variant(const char* str);
   variant(char* str);
   variant(wchar_t* str);
   variant(const wchar_t* str);
   variant(float val);
   variant(uint8_t val);
   variant(int8_t val);
   variant(uint16_t val);
   variant(int16_t val);
   variant(uint32_t val);
   variant(int32_t val);
   variant(uint64_t val);
   variant(int64_t val);
   variant(double val);
   variant(bool val);
   variant(blob val);
   variant(std::string val);
   variant(variant_object);
   variant(mutable_variant_object);
   variant(variants);
   variant(const variant&);
   variant(variant&&);
   ~variant();

   class visitor {
    public:
      virtual ~visitor() {}
      virtual void handle() const = 0;
      virtual void handle(const int64_t& v) const = 0;
      virtual void handle(const uint64_t& v) const = 0;
      virtual void handle(const double& v) const = 0;
      virtual void handle(const bool& v) const = 0;
      virtual void handle(const std::string& v) const = 0;
      virtual void handle(const variant_object& v) const = 0;
      virtual void handle(const variants& v) const = 0;
      virtual void handle(const blob& v) const = 0;
   };

   void visit(const visitor& v) const;
   type_id get_type() const;
   bool is_null() const;
   bool is_string() const;
   bool is_bool() const;
   bool is_int64() const;
   bool is_uint64() const;
   bool is_double() const;
   bool is_object() const;
   bool is_array() const;
   bool is_blob() const;
   bool is_numeric() const;
   bool is_integer() const;
   int64_t as_int64() const;
   uint64_t as_uint64() const;
   bool as_bool() const;
   double as_double() const;
   blob& get_blob();
   const blob& get_blob() const;
   blob as_blob() const;
   std::string as_string() const;
   const std::string& get_string() const;
   variants& get_array();
   const variants& get_array() const;
   variant_object& get_object();
   const variant_object& get_object() const;
   const variant& operator[](const char*) const;
   const variant& operator[](size_t pos) const;
   size_t size() const;
   size_t estimated_size() const;

   template <typename T> T as() const {
      T tmp;
      from_variant(*this, tmp);
      return tmp;
   }
   template <typename T> void as(T& v) const {
      from_variant(*this, v);
   }

   variant& operator=(variant&& v);
   variant& operator=(const variant& v);
   template <typename T> variant& operator=(T&& v) {
      return *this = variant(forge::forward<T>(v));
   }
   template <typename T> explicit variant(const std::optional<T>& v) {
      if (v.has_value()) {
         *this = variant(*v);
      }
   }
   template <typename T> explicit variant(const T& val) {
      to_variant(val, *this);
   }
   template <typename T> explicit variant(const T& val, const forge::yield_function_t& yield) {
      to_variant(val, *this, yield);
   }

   void clear();

 private:
   void init();
   alignas(double) std::array<char, std::max(sizeof(uintmax_t), sizeof(double)) * 2> _data = {};
};

variant operator+(const variant& a, const variant& b);
variant operator-(const variant& a, const variant& b);
variant operator*(const variant& a, const variant& b);
variant operator/(const variant& a, const variant& b);
bool operator==(const variant& a, const variant& b);
bool operator!=(const variant& a, const variant& b);
bool operator<(const variant& a, const variant& b);
bool operator>(const variant& a, const variant& b);
bool operator<=(const variant& a, const variant& b);
bool operator!(const variant& a);

class variant_object {
 public:
   class entry {
    public:
      entry();
      entry(std::string k, variant v);
      entry(entry&& e) noexcept;
      entry(const entry& e);
      entry& operator=(const entry&);
      entry& operator=(entry&&) noexcept;
      const std::string& key() const;
      const variant& value() const;
      variant& value();
      void set(variant v);
      friend bool operator==(const entry& a, const entry& b) {
         return a._key == b._key && a._value == b._value;
      }
      friend bool operator!=(const entry& a, const entry& b) {
         return !(a == b);
      }

    private:
      std::string _key;
      variant _value;
   };

   using iterator = std::vector<entry>::const_iterator;
   iterator begin() const;
   iterator end() const;
   iterator find(const std::string& key) const;
   iterator find(const char* key) const;
   const variant& operator[](const std::string& key) const;
   const variant& operator[](const char* key) const;
   size_t size() const;
   bool contains(const char* key) const {
      return find(key) != end();
   }
   variant_object();
   variant_object(std::string key, variant val);
   template <typename T> variant_object(std::string key, T&& val) {
      *this = variant_object(std::move(key), variant(std::forward<T>(val)));
   }
   variant_object(const variant_object&);
   variant_object(variant_object&&) noexcept;
   variant_object(const mutable_variant_object&);
   variant_object(mutable_variant_object&&);
   variant_object& operator=(variant_object&&) noexcept;
   variant_object& operator=(const variant_object&);
   variant_object& operator=(mutable_variant_object&&);
   variant_object& operator=(const mutable_variant_object&);
   size_t estimated_size() const;

 private:
   std::shared_ptr<std::vector<entry>> _key_value;
   friend class mutable_variant_object;
};

class mutable_variant_object {
 public:
   using entry = variant_object::entry;
   using iterator = std::vector<entry>::iterator;
   using const_iterator = std::vector<entry>::const_iterator;
   iterator begin() const;
   iterator end() const;
   iterator find(const std::string& key) const;
   iterator find(const char* key) const;
   const variant& operator[](const std::string& key) const;
   const variant& operator[](const char* key) const;
   size_t size() const;
   variant& operator[](const std::string& key);
   variant& operator[](const char* key);
   void reserve(size_t s);
   iterator begin();
   iterator end();
   void erase(const std::string& key);
   iterator find(const std::string& key);
   iterator find(const char* key);
   mutable_variant_object& set(std::string key, variant var) &;
   mutable_variant_object set(std::string key, variant var) &&;
   mutable_variant_object& operator()(std::string key, variant var) &;
   mutable_variant_object operator()(std::string key, variant var) &&;
   template <typename T> mutable_variant_object& operator()(std::string key, T&& var) & {
      set(std::move(key), variant(forge::forward<T>(var)));
      return *this;
   }
   template <typename T> mutable_variant_object operator()(std::string key, T&& var) && {
      set(std::move(key), variant(forge::forward<T>(var)));
      return std::move(*this);
   }
   template <typename T> mutable_variant_object operator()(std::string key, const std::initializer_list<T>& val) {
      set(std::move(key), variant(val));
      return std::move(*this);
   }
   mutable_variant_object& operator()(const variant_object& vo) &;
   mutable_variant_object operator()(const variant_object& vo) &&;
   mutable_variant_object& operator()(const mutable_variant_object& mvo) &;
   mutable_variant_object operator()(const mutable_variant_object& mvo) &&;
   explicit mutable_variant_object(variant v) : _key_value(new std::vector<entry>()) {
      *this = v.get_object();
   }
   template <typename T, typename = std::enable_if_t<!std::is_base_of<mutable_variant_object, std::decay_t<T>>::value &&
                                                     !std::is_base_of<variant, std::decay_t<T>>::value &&
                                                     !std::is_base_of<variant_object, std::decay_t<T>>::value>>
   explicit mutable_variant_object(T&& v) : _key_value(new std::vector<entry>()) {
      *this = std::move(variant(forge::forward<T>(v)).get_object());
   }
   mutable_variant_object();
   mutable_variant_object(std::string key, variant val);
   template <typename T> mutable_variant_object(std::string key, T&& val) : _key_value(new std::vector<entry>()) {
      set(std::move(key), variant(std::forward<T>(val)));
   }
   template <typename T>
   mutable_variant_object(std::string key, const std::initializer_list<T>& val) : _key_value(new std::vector<entry>()) {
      set(std::move(key), variant(val));
   }
   mutable_variant_object(mutable_variant_object&&) noexcept;
   mutable_variant_object(const mutable_variant_object&);
   explicit mutable_variant_object(const variant_object&);
   explicit mutable_variant_object(variant_object&&);
   mutable_variant_object& operator=(mutable_variant_object&&) noexcept;
   mutable_variant_object& operator=(const mutable_variant_object&);
   mutable_variant_object& operator=(const variant_object&);
   mutable_variant_object& operator=(variant_object&&);

 private:
   std::unique_ptr<std::vector<entry>> _key_value;
   friend class variant_object;
};
} // namespace forge

export namespace forge {
BOOST_DESCRIBE_STRUCT(blob, (), (data))
}
