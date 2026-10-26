module;

#include <typeindex>

module forge.db.object.registration;

namespace forge::db::object {

registration::registration(forge::db::ids::object_id type, std::type_index model) noexcept
    : type_{type}, model_{model} {}

forge::db::ids::object_id registration::type() const noexcept {
   return type_;
}

std::type_index registration::model() const noexcept {
   return model_;
}

} // namespace forge::db::object
