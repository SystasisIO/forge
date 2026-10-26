module;

#include <typeindex>
#include <typeinfo>

export module forge.db.object.registration;

import forge.db.ids.object_id;
import forge.db.object.index;

export namespace forge::db::object {

// Native model metadata only. Construction retains the typed application-model
// boundary; it is neither a persisted descriptor nor a system registration.
class registration {
 public:
   template <application_object_model Object> [[nodiscard]] static registration of() {
      return registration{object_id_of<Object>::value, std::type_index{typeid(Object)}};
   }

   [[nodiscard]] forge::db::ids::object_id type() const noexcept;
   [[nodiscard]] std::type_index model() const noexcept;

 private:
   registration(forge::db::ids::object_id type, std::type_index model) noexcept;

   const forge::db::ids::object_id type_;
   const std::type_index model_;
};

} // namespace forge::db::object
