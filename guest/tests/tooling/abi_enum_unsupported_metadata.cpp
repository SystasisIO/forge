import forge.contract;

enum class boolean_flag : bool { zero = false, one = true };
enum class signed_wide : __int128 { zero = 0 };
enum class unsigned_wide : unsigned __int128 { zero = 0 };

class [[forge::contract("wideenum")]] wideenum : public forge::contract::context {
 public:
   using context::context;
   [[forge::action]] void submit(boolean_flag boolean, signed_wide signed_value, unsigned_wide unsigned_value) {
      static_cast<void>(boolean);
      static_cast<void>(signed_value);
      static_cast<void>(unsigned_value);
   }
};
