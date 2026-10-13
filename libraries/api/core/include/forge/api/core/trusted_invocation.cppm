module;

#include <memory>
#include <typeindex>
#include <type_traits>
#include <utility>

export module forge.api.core.trusted_invocation;

export import forge.api.core.exceptions;

namespace forge::api::core::detail {
class trusted_invocation_state;
}

export namespace forge::api::core {

class trusted_invocation_builder;

class trusted_invocation {
 public:
   trusted_invocation() = default;
   ~trusted_invocation();

   trusted_invocation(const trusted_invocation&) = default;
   trusted_invocation& operator=(const trusted_invocation&) = default;
   trusted_invocation(trusted_invocation&&) noexcept = default;
   trusted_invocation& operator=(trusted_invocation&&) noexcept = default;

   [[nodiscard]] bool empty() const noexcept;

   template <typename T> [[nodiscard]] const std::remove_cvref_t<T>* find() const noexcept {
      using value_type = std::remove_cvref_t<T>;
      static_assert(std::is_object_v<value_type>, "trusted invocation values must be object types");
      return static_cast<const value_type*>(find_exact(typeid(value_type)));
   }

   template <typename T> [[nodiscard]] bool contains() const noexcept {
      return find<T>() != nullptr;
   }

 private:
   explicit trusted_invocation(std::shared_ptr<const detail::trusted_invocation_state> value) noexcept;
   [[nodiscard]] const void* find_exact(std::type_index type) const noexcept;

   std::shared_ptr<const detail::trusted_invocation_state> state_;

   friend class trusted_invocation_builder;
};

class trusted_invocation_builder {
 public:
   trusted_invocation_builder();
   ~trusted_invocation_builder();

   trusted_invocation_builder(const trusted_invocation_builder&) = delete;
   trusted_invocation_builder& operator=(const trusted_invocation_builder&) = delete;
   trusted_invocation_builder(trusted_invocation_builder&&) noexcept;
   trusted_invocation_builder& operator=(trusted_invocation_builder&&) noexcept;

   template <typename T> trusted_invocation_builder& set(T value) & {
      using value_type = std::remove_cvref_t<T>;
      static_assert(std::is_object_v<value_type>, "trusted invocation values must be object types");
      insert(typeid(value_type), std::make_shared<const value_type>(std::move(value)));
      return *this;
   }

   template <typename T> trusted_invocation_builder&& set(T value) && {
      set(std::move(value));
      return std::move(*this);
   }

   [[nodiscard]] trusted_invocation build() &&;

   trusted_invocation_builder& merge(const trusted_invocation& value) &;
   trusted_invocation_builder&& merge(const trusted_invocation& value) &&;

 private:
   void insert(std::type_index type, std::shared_ptr<const void> value);

   std::unique_ptr<detail::trusted_invocation_state> state_;
};

} // namespace forge::api::core
