import forge.contract;
import forge.tests.tooling.enum_wire.values;

// No pack/unpack call: Abigen must discover the specialization from imported AST.
// Expected aliases/metadata: shared_status=int64; signed/unsigned8/16/32/64 use their matching builtin.
// payload.fixed=uint16[2], narrow_fixed=int16[2], sequence=shared_status[].
// Unopted aliases remain legacy_status=uint16, opted_out_status=uint8, plain_status=uint16.
class [[forge::contract("enumwire")]] enumwire : public forge::contract::context {
 public:
   using context::context;
   [[forge::action]] void submit(forge::tests::tooling::enum_wire::payload value) {
      static_cast<void>(value);
   }
};
