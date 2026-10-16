#pragma once

namespace forge::plugins::db::store {

// One record's closed directory; not a filesystem service or another store map.
// The plugin admits at most one owned compute call at a time.
class activation_state {
 public:
   explicit activation_state(std::filesystem::path root);
   ~activation_state();
   activation_state(const activation_state&) = delete;
   activation_state& operator=(const activation_state&) = delete;

   void prepare();
   void publish(std::filesystem::path destination);
   void confirm();
   [[nodiscard]] activation_status snapshot() const;

 private:
   void verify() const;
   [[nodiscard]] bool matches(const std::string& leaf) const;
   void sync_root_and_parent() const;
   void mark_published(std::filesystem::path root, std::string leaf);

   mutable std::mutex mutex_;
   activation_status status_;
   std::filesystem::path parent_;
   std::string leaf_;
   const std::uint64_t process_;
   int parent_fd_ = -1;
   int root_fd_ = -1;
   int lock_fd_ = -1;
};

} // namespace forge::plugins::db::store
