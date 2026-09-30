module;

#include <boost/asio/awaitable.hpp>

#include <coroutine>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

module forge.crypto.keystore.store;

#include "details/store_impl.hxx"

namespace forge::crypto::keystore {

store::store(std::unique_ptr<impl> implementation) : impl_(std::move(implementation)) {}

store store::create(std::filesystem::path path, core::secret_string password, store_options options) {
   return store{impl::create(std::move(path), std::move(password), options)};
}

store store::open(std::filesystem::path path, core::secret_string password, store_options options) {
   return store{impl::open(std::move(path), std::move(password), options)};
}

store store::create(std::shared_ptr<ownership> owner, core::secret_string password, store_options options) {
   return store{impl::create(std::move(owner), std::move(password), options)};
}

store store::open(std::shared_ptr<ownership> owner, core::secret_string password, store_options options) {
   return store{impl::open(std::move(owner), std::move(password), options)};
}

std::shared_ptr<ownership> store::owner() const noexcept {
   return impl_->owner();
}

store::~store() = default;
store::store(store&&) noexcept = default;
store& store::operator=(store&&) noexcept = default;

void store::put(signer::key_id id, asymmetric::private_key key, bool replace) {
   impl_->put(std::move(id), std::move(key), replace);
}

void store::erase(const signer::key_id& id) {
   impl_->erase(id);
}

void store::save() {
   impl_->save();
}

const std::filesystem::path& store::path() const noexcept {
   return impl_->path();
}

boost::asio::awaitable<std::vector<signer::key_info>> store::keys() {
   co_return keys_sync();
}

boost::asio::awaitable<signer::key_info> store::describe(const signer::key_id& id) {
   co_return describe_sync(id);
}

boost::asio::awaitable<signer::sign_digest_response> store::sign_digest(signer::sign_digest_request request) {
   co_return sign_digest_sync(request);
}

std::vector<signer::key_info> store::keys_sync() {
   return impl_->keys();
}

signer::key_info store::describe_sync(const signer::key_id& id) {
   return impl_->describe(id);
}

signer::sign_digest_response store::sign_digest_sync(const signer::sign_digest_request& request) {
   return impl_->sign_digest(request);
}

} // namespace forge::crypto::keystore
