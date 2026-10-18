#pragma once

// Include after standard headers and the owning file_store module declaration/imports.
namespace forge::swarm::storage::detail {

class file_layout {
 public:
   // The immutable descriptor must outlive this layout and every in-flight operation.
   file_layout(const core::descriptor& descriptor, std::filesystem::path destination,
               std::string storage_key, open_mode mode, bool external_authority = false,
               bool has_external_checkpoint = false);
   ~file_layout();
   file_layout(const file_layout&) = delete;
   file_layout& operator=(const file_layout&) = delete;
   file_layout(file_layout&&) = delete;
   file_layout& operator=(file_layout&&) = delete;

   [[nodiscard]] bool published() const;
   [[nodiscard]] std::vector<std::uint8_t> read(std::uint32_t file, std::uint64_t offset,
                                              std::uint32_t length) const;
   void write(std::uint32_t file, std::uint64_t offset, std::span<const std::uint8_t> bytes);
   void sync_payload();
   void publish();
   void sync_parent();
   [[nodiscard]] std::optional<std::vector<std::uint8_t>> read_checkpoint() const;
   void write_checkpoint(std::span<const std::uint8_t> bytes);
   void assert_current() const;
   void close_checked();
   void close() noexcept;

 private:
   struct identity {
      std::uint64_t device = 0;
      std::uint64_t inode = 0;
      bool operator==(const identity&) const = default;
   };

   struct node {
      std::string name;
      std::uint32_t parent = 0;
      std::uint64_t size = 0;
      identity expected;
      bool directory = false;
      bool recorded = false;
   };

   [[nodiscard]] int open_node(std::uint32_t index, bool create = false) const;
   void check_node(std::uint32_t index, int fd) const;
   void verify_root() const;
   void verify_checkpoint() const;
   void initialize_nodes(bool create);
   void validate_range(std::uint32_t file, std::uint64_t offset, std::size_t length) const;

   const core::descriptor& _descriptor;
   std::vector<node> _nodes;
   std::vector<std::uint32_t> _files;
   std::string _destination_name;
   std::string _stage_name;
   std::string _checkpoint_name;
   std::string _lock_name;
   identity _parent_identity;
   identity _lock_identity;
   std::optional<identity> _checkpoint_identity;
   int _parent = -1;
   int _root = -1;
   int _lock = -1;
   bool _published = false;
   bool _seeded = false;
   bool _has_external_checkpoint = false;
};

} // namespace forge::swarm::storage::detail
