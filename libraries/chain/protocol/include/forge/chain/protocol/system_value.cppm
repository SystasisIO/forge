module;

#include <cstdint>

export module forge.chain.protocol.system:value;

export import forge.chain.protocol.authority;

import forge.raw.codec;

export namespace forge::chain::protocol {

struct newaccount {
   account_name creator;
   account_name name;
   authority owner;
   authority active;

   static constexpr action_name get_name() {
      return make_name("newaccount");
   }
};

struct setcode {
   account_name account;
   std::uint8_t vmtype = 0;
   std::uint8_t vmversion = 0;
   bytes code;

   static constexpr action_name get_name() {
      return make_name("setcode");
   }
};

struct setabi {
   account_name account;
   bytes abi;

   static constexpr action_name get_name() {
      return make_name("setabi");
   }
};

struct updateauth {
   account_name account;
   permission_name permission;
   permission_name parent;
   authority auth;

   static constexpr action_name get_name() {
      return make_name("updateauth");
   }
};

struct deleteauth {
   account_name account;
   permission_name permission;

   static constexpr action_name get_name() {
      return make_name("deleteauth");
   }
};

struct linkauth {
   account_name account;
   account_name code;
   action_name type;
   permission_name requirement;

   static constexpr action_name get_name() {
      return make_name("linkauth");
   }
};

struct unlinkauth {
   account_name account;
   account_name code;
   action_name type;

   static constexpr action_name get_name() {
      return make_name("unlinkauth");
   }
};

struct canceldelay {
   permission_level canceling_auth;
   transaction_id trx_id;

   static constexpr action_name get_name() {
      return make_name("canceldelay");
   }
};

struct onerror {
   uint128_t sender_id = 0;
   bytes sent_trx;

   static constexpr action_name get_name() {
      return make_name("onerror");
   }
};

struct claimrewards {
   account_name owner;

   static constexpr action_name get_name() {
      return make_name("claimrewards");
   }
};

template <typename Stream> void raw_pack(Stream& stream, const newaccount& value) {
   forge::raw::pack(stream, value.creator);
   forge::raw::pack(stream, value.name);
   forge::raw::pack(stream, value.owner);
   forge::raw::pack(stream, value.active);
}

template <typename Stream> void raw_unpack(Stream& stream, newaccount& value) {
   forge::raw::unpack(stream, value.creator);
   forge::raw::unpack(stream, value.name);
   forge::raw::unpack(stream, value.owner);
   forge::raw::unpack(stream, value.active);
}

template <typename Stream> void raw_pack(Stream& stream, const setcode& value) {
   forge::raw::pack(stream, value.account);
   forge::raw::pack(stream, value.vmtype);
   forge::raw::pack(stream, value.vmversion);
   forge::raw::pack(stream, value.code);
}

template <typename Stream> void raw_unpack(Stream& stream, setcode& value) {
   forge::raw::unpack(stream, value.account);
   forge::raw::unpack(stream, value.vmtype);
   forge::raw::unpack(stream, value.vmversion);
   forge::raw::unpack(stream, value.code);
}

template <typename Stream> void raw_pack(Stream& stream, const setabi& value) {
   forge::raw::pack(stream, value.account);
   forge::raw::pack(stream, value.abi);
}

template <typename Stream> void raw_unpack(Stream& stream, setabi& value) {
   forge::raw::unpack(stream, value.account);
   forge::raw::unpack(stream, value.abi);
}

template <typename Stream> void raw_pack(Stream& stream, const updateauth& value) {
   forge::raw::pack(stream, value.account);
   forge::raw::pack(stream, value.permission);
   forge::raw::pack(stream, value.parent);
   forge::raw::pack(stream, value.auth);
}

template <typename Stream> void raw_unpack(Stream& stream, updateauth& value) {
   forge::raw::unpack(stream, value.account);
   forge::raw::unpack(stream, value.permission);
   forge::raw::unpack(stream, value.parent);
   forge::raw::unpack(stream, value.auth);
}

template <typename Stream> void raw_pack(Stream& stream, const deleteauth& value) {
   forge::raw::pack(stream, value.account);
   forge::raw::pack(stream, value.permission);
}

template <typename Stream> void raw_unpack(Stream& stream, deleteauth& value) {
   forge::raw::unpack(stream, value.account);
   forge::raw::unpack(stream, value.permission);
}

template <typename Stream> void raw_pack(Stream& stream, const linkauth& value) {
   forge::raw::pack(stream, value.account);
   forge::raw::pack(stream, value.code);
   forge::raw::pack(stream, value.type);
   forge::raw::pack(stream, value.requirement);
}

template <typename Stream> void raw_unpack(Stream& stream, linkauth& value) {
   forge::raw::unpack(stream, value.account);
   forge::raw::unpack(stream, value.code);
   forge::raw::unpack(stream, value.type);
   forge::raw::unpack(stream, value.requirement);
}

template <typename Stream> void raw_pack(Stream& stream, const unlinkauth& value) {
   forge::raw::pack(stream, value.account);
   forge::raw::pack(stream, value.code);
   forge::raw::pack(stream, value.type);
}

template <typename Stream> void raw_unpack(Stream& stream, unlinkauth& value) {
   forge::raw::unpack(stream, value.account);
   forge::raw::unpack(stream, value.code);
   forge::raw::unpack(stream, value.type);
}

template <typename Stream> void raw_pack(Stream& stream, const canceldelay& value) {
   forge::raw::pack(stream, value.canceling_auth);
   forge::raw::pack(stream, value.trx_id);
}

template <typename Stream> void raw_unpack(Stream& stream, canceldelay& value) {
   forge::raw::unpack(stream, value.canceling_auth);
   forge::raw::unpack(stream, value.trx_id);
}

template <typename Stream> void raw_pack(Stream& stream, const onerror& value) {
   forge::raw::pack(stream, value.sender_id);
   forge::raw::pack(stream, value.sent_trx);
}

template <typename Stream> void raw_unpack(Stream& stream, onerror& value) {
   forge::raw::unpack(stream, value.sender_id);
   forge::raw::unpack(stream, value.sent_trx);
}

template <typename Stream> void raw_pack(Stream& stream, const claimrewards& value) {
   forge::raw::pack(stream, value.owner);
}

template <typename Stream> void raw_unpack(Stream& stream, claimrewards& value) {
   forge::raw::unpack(stream, value.owner);
}

} // namespace forge::chain::protocol
