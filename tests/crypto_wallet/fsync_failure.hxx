#pragma once

#include <filesystem>

namespace forge::tests::wallet {

// Test executable only: fail fsync of this directory after the next successful
// regular-file fsync. There are no test switches in the production keystore.
void fail_commit_sync(const std::filesystem::path& directory);
bool commit_sync_failed();
void clear_sync_failure();

} // namespace forge::tests::wallet
