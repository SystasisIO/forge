module;

// Donor-derived lowering patterns: Spring LLVMEmitIR.cpp, pinned in the
// accompanying provenance note. WAVM BSD-3-Clause notice: LICENSE.WAVM.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <forge/exceptions/macros.hpp>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>

module forge.tooling.wasm_compiler.compiler;

import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.optimizing.exceptions;

#include "details/lowering.hxx"
#include "details/control_flow.hxx"

namespace forge::tooling::wasm_compiler {
namespace {
namespace interpret = vm::wasm::interpret;
namespace optimizing = vm::wasm::optimizing;
using abi = optimizing::invocation_abi;

// Callback signatures, like context offsets below, derive from the one shared
// C++ ABI. There is no separately maintained LLVM struct or callback prototype.
template <typename T> llvm::Type* abi_type(llvm::LLVMContext& context) {
   if constexpr (std::is_pointer_v<T>)
      return llvm::PointerType::getUnqual(context);
   else if constexpr (std::is_void_v<T>)
      return llvm::Type::getVoidTy(context);
   else if constexpr (std::is_enum_v<T>)
      return abi_type<std::underlying_type_t<T>>(context);
   else {
      static_assert(std::is_integral_v<T>);
      return llvm::IntegerType::get(context, sizeof(T) * 8);
   }
}
template <typename T> struct abi_signature;
template <typename R, typename... Args> struct abi_signature<R (*)(Args...) noexcept> {
   static llvm::FunctionType* get(llvm::LLVMContext& context) {
      return llvm::FunctionType::get(abi_type<R>(context), {abi_type<Args>(context)...}, false);
   }
};

struct emitter {
   const interpret::module& parsed;
   const optimizing::artifact_payload& metadata;
   std::span<const std::uint32_t> canonical_types;
   llvm::LLVMContext context;
   llvm::Module output{"forge_wasm", context};
   llvm::IRBuilder<> builder{context};
   std::vector<llvm::Function*> functions;
   llvm::Function* current = nullptr;
   llvm::Value* frame_bytes = nullptr;
   llvm::AllocaInst* operands = nullptr;
   std::uint32_t height = 0;
   std::vector<llvm::AllocaInst*> locals;

   emitter(const interpret::module& source, const optimizing::artifact_payload& image,
           std::span<const std::uint32_t> types) : parsed(source), metadata(image), canonical_types(types) {
      for (std::uint32_t i = 0; i < parsed.get_functions_total(); ++i) {
         auto* function =
             llvm::Function::Create(abi_signature<optimizing::entry_function>::get(context),
                                    llvm::GlobalValue::ExternalLinkage, "forge_wasm_" + std::to_string(i), output);
         function->addFnAttr(llvm::Attribute::NoUnwind);
         function->addFnAttr(llvm::Attribute::NoInline);
         function->addFnAttr(llvm::Attribute::NoRedZone);
         function->addFnAttr("disable-tail-calls", "true");
         functions.push_back(function);
      }
   }
   llvm::Value* pointer(llvm::Value* base, std::size_t offset) {
      return builder.CreateGEP(builder.getInt8Ty(), base, builder.getInt64(offset));
   }
   template <typename T> llvm::Value* load(llvm::Value* base, std::size_t offset) {
      return builder.CreateLoad(abi_type<T>(context), pointer(base, offset));
   }
   void store(llvm::Value* base, std::size_t offset, llvm::Value* value) {
      builder.CreateStore(value, pointer(base, offset));
   }
   llvm::Value* invocation() {
      return current->getArg(0);
   }
   template <typename Callback> llvm::CallInst* callback(std::size_t offset, llvm::ArrayRef<llvm::Value*> arguments) {
      auto* callbacks = load<const optimizing::invocation_callbacks*>(invocation(), offsetof(abi, callbacks));
      auto* function = load<Callback>(callbacks, offset);
      auto* call = builder.CreateCall(abi_signature<Callback>::get(context), function, arguments);
      call->setDoesNotThrow();
      return call;
   }
   void trap(llvm::Value* reason) {
      callback<optimizing::trap_callback>(offsetof(optimizing::invocation_callbacks, trap), {invocation(), reason})
          ->setDoesNotReturn();
      builder.CreateUnreachable();
   }
   void guard(llvm::Value* bad, llvm::Value* reason) {
      auto* failed = llvm::BasicBlock::Create(context, "trap", current);
      auto* safe = llvm::BasicBlock::Create(context, "safe", current);
      builder.CreateCondBr(bad, failed, safe);
      builder.SetInsertPoint(failed);
      trap(reason);
      builder.SetInsertPoint(safe);
   }
   void guard(llvm::Value* bad, optimizing::trap_reason reason) {
      guard(bad, builder.getInt32(static_cast<std::uint32_t>(reason)));
   }
   void poll() {
      auto* epoch = load<const std::uint64_t*>(invocation(), offsetof(abi, epoch));
      auto* observed = builder.CreateLoad(builder.getInt64Ty(), epoch, "epoch");
      observed->setAtomic(llvm::AtomicOrdering::Acquire, llvm::SyncScope::System);
      observed->setAlignment(llvm::Align{std::atomic_ref<std::uint64_t>::required_alignment});
      // Plain threshold belongs to the suspended logical invocation thread.
      // No readonly/invariant metadata or cached SSA threshold across bridges:
      // every check reloads it, so a successful slow rearm/nested return is seen.
      auto* deadline = load<std::uint64_t>(invocation(), offsetof(abi, epoch_deadline));
      auto* slow = llvm::BasicBlock::Create(context, "epoch.slow", current);
      auto* done = llvm::BasicBlock::Create(context, "epoch.continue", current);
      builder.CreateCondBr(builder.CreateICmpUGE(observed, deadline), slow, done);
      builder.SetInsertPoint(slow);
      auto* reason =
          callback<optimizing::poll_callback>(offsetof(optimizing::invocation_callbacks, poll), {invocation()});
      reason->addFnAttr(llvm::Attribute::Cold);
      guard(builder.CreateICmpNE(reason, builder.getInt32(0)), reason);
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
   }
   llvm::IntegerType* raw_type(interpret::value_type type) {
      if (type == interpret::i32 || type == interpret::f32)
         return builder.getInt32Ty();
      if (type == interpret::i64 || type == interpret::f64)
         return builder.getInt64Ty();
      FORGE_THROW_EXCEPTION(optimizing::exceptions::compiler_unavailable,
                            "value is outside the canonical raw numeric types");
   }
   llvm::Value* memory_address(std::uint32_t offset, std::uint32_t width) {
      // The MVP address and immediate are unsigned i32 values. Their sum plus
      // the maximum eight-byte access fits i64, never wrapping back into guest
      // memory. Do not form a guest pointer before checking the entire access.
      auto* address = builder.CreateZExt(pop(builder.getInt32Ty()), builder.getInt64Ty());
      auto* effective = builder.CreateAdd(address, builder.getInt64(offset));
      auto* bytes = load<std::uint64_t>(invocation(), offsetof(abi, memory_bytes));
      guard(builder.CreateICmpUGT(builder.CreateAdd(effective, builder.getInt64(width)), bytes),
            optimizing::trap_reason::memory_bounds);
      // No cached base survives a generated call/callback, including growth.
      auto* base = load<std::uint8_t*>(invocation(), offsetof(abi, memory_base));
      return builder.CreateGEP(builder.getInt8Ty(), base, effective);
   }
   void memory_load(std::uint32_t offset, std::uint32_t width, bool wide, bool sign) {
      auto* address = memory_address(offset, width);
      auto* value = builder.CreateLoad(builder.getIntNTy(width * 8), address);
      value->setAlignment(llvm::Align{1});
      value->setVolatile(true);
      auto* type = wide ? builder.getInt64Ty() : builder.getInt32Ty();
      push(sign ? builder.CreateSExtOrTrunc(value, type) : builder.CreateZExtOrTrunc(value, type));
   }
   void memory_store(std::uint32_t offset, std::uint32_t width, bool wide) {
      auto* value = pop(wide ? builder.getInt64Ty() : builder.getInt32Ty());
      auto* address = memory_address(offset, width);
      auto* instruction =
          builder.CreateStore(builder.CreateTruncOrBitCast(value, builder.getIntNTy(width * 8)), address);
      instruction->setAlignment(llvm::Align{1});
      instruction->setVolatile(true);
   }
   void memory(const interpret::opcode& value) {
      // Canonical writer fields are flags_align, offset. Alignment is a hint,
      // never an LLVM promise: even naturally hinted accesses may be unaligned.
      switch (static_cast<interpret::opcodes>(value.index())) {
#define LOAD(name, width, wide, sign)                                                                                  \
   case interpret::name:                                                                                               \
      memory_load(value.get<interpret::name##_t>().offset, width, wide, sign);                                         \
      break;
         LOAD(i32_load, 4, false, false)
         LOAD(i64_load, 8, true, false)
         LOAD(f32_load, 4, false, false)
         LOAD(f64_load, 8, true, false)
         LOAD(i32_load8_s, 1, false, true)
         LOAD(i32_load8_u, 1, false, false)
         LOAD(i32_load16_s, 2, false, true)
         LOAD(i32_load16_u, 2, false, false)
         LOAD(i64_load8_s, 1, true, true)
         LOAD(i64_load8_u, 1, true, false)
         LOAD(i64_load16_s, 2, true, true)
         LOAD(i64_load16_u, 2, true, false)
         LOAD(i64_load32_s, 4, true, true)
         LOAD(i64_load32_u, 4, true, false)
#undef LOAD
#define STORE(name, width, wide)                                                                                       \
   case interpret::name:                                                                                               \
      memory_store(value.get<interpret::name##_t>().offset, width, wide);                                              \
      break;
         STORE(i32_store, 4, false)
         STORE(i64_store, 8, true)
         STORE(f32_store, 4, false)
         STORE(f64_store, 8, true)
         STORE(i32_store8, 1, false)
         STORE(i32_store16, 2, false)
         STORE(i64_store8, 1, true)
         STORE(i64_store16, 2, true)
         STORE(i64_store32, 4, true)
#undef STORE
      default:
         FORGE_THROW_EXCEPTION(optimizing::exceptions::invalid_module, "unexpected canonical memory operation");
      }
   }
   void get_global(std::uint32_t index) {
      auto* globals = load<interpret::expr_value*>(invocation(), offsetof(abi, globals));
      auto* bits = load<std::uint64_t>(globals, index * sizeof(interpret::expr_value));
      push(builder.CreateTruncOrBitCast(bits, raw_type(parsed.globals[index].type.content_type)));
   }
   void set_global(std::uint32_t index) {
      auto* value = pop(raw_type(parsed.globals[index].type.content_type));
      auto* globals = load<interpret::expr_value*>(invocation(), offsetof(abi, globals));
      store(globals, index * sizeof(interpret::expr_value), builder.CreateZExtOrBitCast(value, builder.getInt64Ty()));
   }
   llvm::Value* operand(std::uint32_t index) {
      return builder.CreateGEP(builder.getInt64Ty(), operands, builder.getInt32(index));
   }
   void push(llvm::Value* value) {
      builder.CreateStore(builder.CreateZExtOrBitCast(value, builder.getInt64Ty()), operand(height++));
   }
   llvm::Value* pop(llvm::Type* type = nullptr) {
      if (height == 0)
         FORGE_THROW_EXCEPTION(optimizing::exceptions::invalid_module, "typed lowering operand underflow");
      auto* result = builder.CreateLoad(builder.getInt64Ty(), operand(--height));
      return type ? builder.CreateTruncOrBitCast(result, type) : result;
   }
   llvm::AllocaInst* allocate(llvm::Type* type, std::uint32_t count = 1) {
      auto allocation = llvm::IRBuilder<>{&current->getEntryBlock(), current->getEntryBlock().begin()};
      return allocation.CreateAlloca(type, builder.getInt32(count));
   }
   void prologue(std::uint32_t index) {
      // Reject an obsolete/short context before reading appended ABI2 fields.
      // Production activation admission also checks this before native entry.
      auto* wrong_version = builder.CreateICmpNE(
          load<std::uint32_t>(invocation(), offsetof(abi, version)), builder.getInt32(abi::current_version));
      auto* wrong_size = builder.CreateICmpNE(
          load<std::uint32_t>(invocation(), offsetof(abi, size)), builder.getInt32(sizeof(abi)));
      guard(builder.CreateOr(wrong_version, wrong_size), optimizing::trap_reason::host_exception);
      auto* missing_epoch = builder.CreateIsNull(load<const std::uint64_t*>(invocation(), offsetof(abi, epoch)));
      auto* missing_reason = builder.CreateIsNull(load<const std::uint32_t*>(invocation(), offsetof(abi, sticky_reason)));
      guard(builder.CreateOr(missing_epoch, missing_reason), optimizing::trap_reason::host_exception);
      poll();
      auto* depth = load<std::uint32_t>(invocation(), offsetof(abi, call_depth));
      auto* maximum = load<std::uint32_t>(invocation(), offsetof(abi, max_call_depth));
      guard(builder.CreateICmpUGE(depth, maximum), optimizing::trap_reason::stack_overflow);
      auto* metadata = load<const optimizing::native_function*>(invocation(), offsetof(abi, functions));
      frame_bytes = load<std::uint64_t>(metadata, index * sizeof(optimizing::native_function) +
                                                      offsetof(optimizing::native_function, stack_bytes));
      auto* remaining = load<std::uint64_t>(invocation(), offsetof(abi, stack_remaining_bytes));
      // This allowance comes from the local invocation's native stack, not
      // canonical max_call_depth. Never turn local capacity into a WASM trap.
      guard(builder.CreateICmpULT(remaining, frame_bytes), optimizing::trap_reason::native_stack_exhausted);
      store(invocation(), offsetof(abi, call_depth), builder.CreateAdd(depth, builder.getInt32(1)));
      store(invocation(), offsetof(abi, stack_remaining_bytes), builder.CreateSub(remaining, frame_bytes));
   }
   void epilogue(const interpret::func_type& type) {
      if (type.return_count)
         builder.CreateStore(pop(), current->getArg(2));
      auto* remaining = load<std::uint64_t>(invocation(), offsetof(abi, stack_remaining_bytes));
      auto* depth = load<std::uint32_t>(invocation(), offsetof(abi, call_depth));
      store(invocation(), offsetof(abi, stack_remaining_bytes), builder.CreateAdd(remaining, frame_bytes));
      store(invocation(), offsetof(abi, call_depth), builder.CreateSub(depth, builder.getInt32(1)));
      builder.CreateRetVoid();
   }
   void direct_call(std::uint32_t index) {
      const auto& type = parsed.get_function_type(index);
      auto* arguments = allocate(builder.getInt64Ty(), std::max<std::uint32_t>(1, type.param_types.size()));
      for (std::uint32_t i = type.param_types.size(); i > 0; --i)
         builder.CreateStore(pop(), builder.CreateGEP(builder.getInt64Ty(), arguments, builder.getInt32(i - 1)));
      auto* result = allocate(builder.getInt64Ty());
      builder.CreateCall(functions.at(index), {invocation(), arguments, result});
      if (type.return_count)
         push(builder.CreateTruncOrBitCast(builder.CreateLoad(builder.getInt64Ty(), result),
                                           raw_type(type.return_type)));
   }
   void numeric(interpret::opcodes operation, std::uint32_t arity) {
      auto* arguments = allocate(builder.getInt64Ty(), arity);
      for (std::uint32_t i = arity; i > 0; --i)
         builder.CreateStore(pop(), builder.CreateGEP(builder.getInt64Ty(), arguments, builder.getInt32(i - 1)));
      auto* result = allocate(builder.getInt64Ty());
      builder.CreateStore(builder.getInt64(0), result);
      auto* reason = callback<optimizing::numeric_callback>(offsetof(optimizing::invocation_callbacks, numeric),
          {invocation(), builder.getInt32(static_cast<std::uint32_t>(operation)), builder.getInt32(arity), arguments, result});
      guard(builder.CreateICmpNE(reason, builder.getInt32(0)), reason);
      push(builder.CreateLoad(builder.getInt64Ty(), result));
   }
   void import_thunk(std::uint32_t index) {
      const auto& type = parsed.get_function_type(index);
      if (type.return_count)
         builder.CreateStore(builder.getInt64(0), current->getArg(2));
      auto* reason = callback<optimizing::host_call_callback>(offsetof(optimizing::invocation_callbacks, host_call),
          {invocation(), builder.getInt32(metadata.functions[index].import_index), current->getArg(1), current->getArg(2)});
      guard(builder.CreateICmpNE(reason, builder.getInt32(0)), reason);
      if (type.return_count)
         push(builder.CreateTruncOrBitCast(builder.CreateLoad(builder.getInt64Ty(), current->getArg(2)),
                                           raw_type(type.return_type)));
      epilogue(type);
   }
   void indirect_call(std::uint32_t type_index) {
      const auto& type = parsed.types[type_index];
      auto* index = pop(builder.getInt32Ty());
      auto* count = load<std::uint32_t>(invocation(), offsetof(abi, table_count));
      guard(builder.CreateICmpUGE(index, count), optimizing::trap_reason::table_bounds);
      auto* table = load<const optimizing::table_entry*>(invocation(), offsetof(abi, table));
      auto* slot = builder.CreateGEP(
          builder.getInt8Ty(), table,
          builder.CreateMul(builder.CreateZExt(index, builder.getInt64Ty()),
                             builder.getInt64(sizeof(optimizing::table_entry))));
      auto* function_index = load<std::uint32_t>(slot, offsetof(optimizing::table_entry, function_index));
      // A missing entry is rejected before type/function-table dereferences.
      guard(builder.CreateICmpEQ(function_index, builder.getInt32(UINT32_MAX)), optimizing::trap_reason::table_bounds);
      auto* expected = builder.getInt32(canonical_types[type_index]);
      guard(builder.CreateICmpNE(load<std::uint32_t>(slot, offsetof(optimizing::table_entry, type_index)), expected),
            optimizing::trap_reason::indirect_type);
      guard(builder.CreateICmpUGE(function_index, load<std::uint32_t>(invocation(), offsetof(abi, function_count))),
            optimizing::trap_reason::table_bounds);
      auto* functions_base = load<const optimizing::native_function*>(invocation(), offsetof(abi, functions));
      auto* function = builder.CreateGEP(
          builder.getInt8Ty(), functions_base,
          builder.CreateMul(builder.CreateZExt(function_index, builder.getInt64Ty()),
                             builder.getInt64(sizeof(optimizing::native_function))));
      guard(builder.CreateICmpNE(load<std::uint32_t>(function, offsetof(optimizing::native_function, type_index)), expected),
            optimizing::trap_reason::indirect_type);
      auto* entry = load<optimizing::entry_function>(function, offsetof(optimizing::native_function, entry));
      guard(builder.CreateIsNull(entry), optimizing::trap_reason::indirect_type);
      auto* arguments = allocate(builder.getInt64Ty(), std::max<std::uint32_t>(1, type.param_types.size()));
      for (std::uint32_t i = type.param_types.size(); i > 0; --i)
         builder.CreateStore(pop(), builder.CreateGEP(builder.getInt64Ty(), arguments, builder.getInt32(i - 1)));
      auto* result = allocate(builder.getInt64Ty());
      builder.CreateStore(builder.getInt64(0), result);
      auto* call = builder.CreateCall(abi_signature<optimizing::entry_function>::get(context), entry,
                                      {invocation(), arguments, result});
      call->setDoesNotThrow();
      if (type.return_count)
         push(builder.CreateTruncOrBitCast(builder.CreateLoad(builder.getInt64Ty(), result), raw_type(type.return_type)));
   }
   llvm::BasicBlock* edge(const control_flow& flow, const std::vector<llvm::BasicBlock*>& blocks, std::uint32_t pc,
                          std::uint32_t destination, std::uint32_t pop_info = 0) {
      const auto target = flow.target(destination);
      const auto after = control_flow::branch_height(height, pop_info);
      if (flow.nodes[target].height != after)
         FORGE_THROW_EXCEPTION(optimizing::exceptions::invalid_module, "typed lowering branch height mismatch");
      const auto preserve = (pop_info & 0x80000000U) != 0;
      if (target > pc && (!preserve || after == height))
         return blocks[target];
      auto* previous = builder.GetInsertBlock();
      auto* transfer = llvm::BasicBlock::Create(context, "edge", current);
      builder.SetInsertPoint(transfer);
      if (preserve && after != height) {
         auto* result = builder.CreateLoad(builder.getInt64Ty(), operand(height - 1));
         builder.CreateStore(result, operand(after - 1));
      }
      // Function entry and actual backward edges check the atomic epoch. No
      // duplicate caller-edge poll: the callee/import thunk checks its entry.
      if (target <= pc)
         poll();
      builder.CreateBr(blocks[target]);
      builder.SetInsertPoint(previous);
      return transfer;
   }
   llvm::Value* signed_overflow(llvm::Value* left, llvm::Value* right) {
      auto* type = llvm::cast<llvm::IntegerType>(left->getType());
      auto* minimum = llvm::ConstantInt::get(context, llvm::APInt::getSignedMinValue(type->getBitWidth()));
      return builder.CreateAnd(builder.CreateICmpEQ(left, minimum),
                               builder.CreateICmpEQ(right, llvm::ConstantInt::getSigned(type, -1)));
   }
   llvm::Value* signed_remainder(llvm::Value* left, llvm::Value* right) {
      // Spring/WAVM emitSRem algorithm: LLVM srem MIN%-1 is poison, but WASM
      // defines zero. Branch around it rather than evaluating a poisoned value.
      auto* exceptional = builder.GetInsertBlock();
      auto* ordinary = llvm::BasicBlock::Create(context, "remainder", current);
      auto* end = llvm::BasicBlock::Create(context, "remainder.end", current);
      builder.CreateCondBr(signed_overflow(left, right), end, ordinary);
      builder.SetInsertPoint(ordinary);
      auto* value = builder.CreateSRem(left, right);
      builder.CreateBr(end);
      builder.SetInsertPoint(end);
      auto* result = builder.CreatePHI(left->getType(), 2);
      result->addIncoming(llvm::ConstantInt::get(left->getType(), 0), exceptional);
      result->addIncoming(value, ordinary);
      return result;
   }
   void integer(interpret::opcodes operation) {
      if (operation == interpret::i32_wrap_i64) {
         push(builder.CreateTrunc(pop(), builder.getInt32Ty()));
         return;
      }
      if (operation == interpret::i64_extend_s_i32 || operation == interpret::i64_extend_u_i32) {
         auto* value = pop(builder.getInt32Ty());
         push(operation == interpret::i64_extend_s_i32 ? builder.CreateSExt(value, builder.getInt64Ty())
                                                       : builder.CreateZExt(value, builder.getInt64Ty()));
         return;
      }
      const auto wide = operation == interpret::i64_eqz ||
                        (operation >= interpret::i64_eq && operation <= interpret::i64_ge_u) ||
                        (operation >= interpret::i64_clz && operation <= interpret::i64_rotr);
      auto* type = wide ? builder.getInt64Ty() : builder.getInt32Ty();
      auto* right = pop(type);
      if (operation == interpret::i32_eqz || operation == interpret::i64_eqz) {
         push(builder.CreateZExt(builder.CreateICmpEQ(right, llvm::ConstantInt::get(type, 0)), builder.getInt32Ty()));
         return;
      }
      if ((operation >= interpret::i32_clz && operation <= interpret::i32_popcnt) ||
          (operation >= interpret::i64_clz && operation <= interpret::i64_popcnt)) {
         const auto offset = operation - (wide ? interpret::i64_clz : interpret::i32_clz);
         const auto intrinsic =
             offset == 0 ? llvm::Intrinsic::ctlz : (offset == 1 ? llvm::Intrinsic::cttz : llvm::Intrinsic::ctpop);
         push(offset == 2 ? builder.CreateUnaryIntrinsic(intrinsic, right)
                          : builder.CreateIntrinsic(intrinsic, {type}, {right, builder.getFalse()}));
         return;
      }
      auto* left = pop(type);
      if ((operation >= interpret::i32_eq && operation <= interpret::i32_ge_u) ||
          (operation >= interpret::i64_eq && operation <= interpret::i64_ge_u)) {
         const auto predicates = std::array{llvm::CmpInst::ICMP_EQ,  llvm::CmpInst::ICMP_NE,  llvm::CmpInst::ICMP_SLT,
                                            llvm::CmpInst::ICMP_ULT, llvm::CmpInst::ICMP_SGT, llvm::CmpInst::ICMP_UGT,
                                            llvm::CmpInst::ICMP_SLE, llvm::CmpInst::ICMP_ULE, llvm::CmpInst::ICMP_SGE,
                                            llvm::CmpInst::ICMP_UGE};
         const auto offset = operation - (wide ? interpret::i64_eq : interpret::i32_eq);
         push(builder.CreateZExt(builder.CreateICmp(predicates[offset], left, right), builder.getInt32Ty()));
         return;
      }
      // The canonical catalog lists the i32/i64 binary families in the same
      // order. This local normalization is not an independent public opcode ID.
      if (wide)
         operation = static_cast<interpret::opcodes>(operation - interpret::i64_add + interpret::i32_add);
      const auto masked = [&] {
         return builder.CreateAnd(right, llvm::ConstantInt::get(type, type->getBitWidth() - 1));
      };
      switch (operation) {
      case interpret::i32_add:
         push(builder.CreateAdd(left, right));
         break;
      case interpret::i32_sub:
         push(builder.CreateSub(left, right));
         break;
      case interpret::i32_mul:
         push(builder.CreateMul(left, right));
         break;
      case interpret::i32_and:
         push(builder.CreateAnd(left, right));
         break;
      case interpret::i32_or:
         push(builder.CreateOr(left, right));
         break;
      case interpret::i32_xor:
         push(builder.CreateXor(left, right));
         break;
      case interpret::i32_shl:
         push(builder.CreateShl(left, masked()));
         break;
      case interpret::i32_shr_s:
         push(builder.CreateAShr(left, masked()));
         break;
      case interpret::i32_shr_u:
         push(builder.CreateLShr(left, masked()));
         break;
      case interpret::i32_rotl:
         push(builder.CreateIntrinsic(llvm::Intrinsic::fshl, {type}, {left, left, right}));
         break;
      case interpret::i32_rotr:
         push(builder.CreateIntrinsic(llvm::Intrinsic::fshr, {type}, {left, left, right}));
         break;
      case interpret::i32_div_s:
      case interpret::i32_div_u:
      case interpret::i32_rem_s:
      case interpret::i32_rem_u:
         guard(builder.CreateICmpEQ(right, llvm::ConstantInt::get(type, 0)),
               optimizing::trap_reason::integer_divide_by_zero);
         if (operation == interpret::i32_div_s) {
            guard(signed_overflow(left, right), optimizing::trap_reason::integer_overflow);
            push(builder.CreateSDiv(left, right));
         } else if (operation == interpret::i32_div_u)
            push(builder.CreateUDiv(left, right));
         else if (operation == interpret::i32_rem_s)
            push(signed_remainder(left, right));
         else
            push(builder.CreateURem(left, right));
         break;
      default:
         FORGE_THROW_EXCEPTION(optimizing::exceptions::compiler_unavailable,
                               "operation lowering is not available in this compiler slice");
      }
   }
   void lower() {
      auto module_base = std::uint32_t{0};
      const auto imported = parsed.get_imported_functions_size();
      for (std::uint32_t i = 0; i < imported; ++i) {
         current = functions[i];
         builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", current));
         prologue(i);
         operands = allocate(builder.getInt64Ty());
         height = 0;
         import_thunk(i);
      }
      for (std::uint32_t i = 0; i < parsed.code.size(); ++i) {
         const auto function_index = imported + i;
         current = functions[function_index];
         locals.clear();
         builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", current));
         prologue(function_index);
         const auto& type = parsed.get_function_type(function_index);
         if (type.return_count)
            static_cast<void>(raw_type(type.return_type));
         for (std::uint32_t parameter = 0; parameter < type.param_types.size(); ++parameter) {
            auto* local = allocate(raw_type(type.param_types[parameter]));
            auto* value = builder.CreateLoad(builder.getInt64Ty(),
                                             pointer(current->getArg(1), parameter * sizeof(interpret::expr_value)));
            builder.CreateStore(builder.CreateTruncOrBitCast(value, local->getAllocatedType()), local);
            locals.push_back(local);
         }
         const auto& body = parsed.code[i];
         const auto flow = control_flow{parsed, i, module_base};
         module_base += body.size; // Checked once by the canonical opcode traversal.
         operands = allocate(builder.getInt64Ty(), std::max<std::uint32_t>(1, flow.max_stack));
         for (std::uint32_t local_index = 0; local_index < body.locals.size(); ++local_index) {
            const auto& entry = body.locals[local_index];
            for (std::uint32_t count = 0; count < entry.count; ++count) {
               auto* local = allocate(raw_type(entry.type));
               builder.CreateStore(llvm::ConstantInt::get(local->getAllocatedType(), 0), local);
               locals.push_back(local);
            }
         }
         auto blocks = std::vector<llvm::BasicBlock*>(body.size, nullptr);
         for (std::uint32_t pc = 0; pc < body.size; ++pc)
            if (flow.nodes[pc].height != control_flow::unreachable)
               blocks[pc] = llvm::BasicBlock::Create(context, "pc." + std::to_string(pc), current);
         builder.CreateBr(blocks[0]);
         for (std::uint32_t pc = 0; pc < body.size; ++pc) {
            if (!blocks[pc])
               continue;
            height = flow.nodes[pc].height;
            builder.SetInsertPoint(blocks[pc]);
            const auto& op = body.code[pc];
            if (op.is_a<interpret::get_local_t>())
               push(builder.CreateLoad(locals.at(op.get<interpret::get_local_t>().index)->getAllocatedType(),
                                       locals.at(op.get<interpret::get_local_t>().index)));
            else if (op.is_a<interpret::set_local_t>()) {
               auto* local = locals.at(op.get<interpret::set_local_t>().index);
               builder.CreateStore(pop(local->getAllocatedType()), local);
            } else if (op.is_a<interpret::tee_local_t>()) {
               auto* local = locals.at(op.get<interpret::tee_local_t>().index);
               auto* value = pop(local->getAllocatedType());
               builder.CreateStore(value, local);
               push(value);
            } else if (op.is_a<interpret::i32_const_t>())
               push(builder.getInt32(op.get<interpret::i32_const_t>().data.ui));
            else if (op.is_a<interpret::i64_const_t>())
               push(builder.getInt64(op.get<interpret::i64_const_t>().data.ui));
            else if (op.is_a<interpret::f32_const_t>())
               push(builder.getInt32(std::bit_cast<std::uint32_t>(op.get<interpret::f32_const_t>().data)));
            else if (op.is_a<interpret::f64_const_t>())
               push(builder.getInt64(std::bit_cast<std::uint64_t>(op.get<interpret::f64_const_t>().data)));
            else if (op.is_a<interpret::get_global_t>())
               get_global(op.get<interpret::get_global_t>().index);
            else if (op.is_a<interpret::set_global_t>())
               set_global(op.get<interpret::set_global_t>().index);
            else if (op.index() >= interpret::i32_load && op.index() <= interpret::i64_store32)
               memory(op);
            else if (op.is_a<interpret::current_memory_t>())
               push(load<std::uint32_t>(invocation(), offsetof(abi, current_pages)));
            else if (op.is_a<interpret::grow_memory_t>()) {
               auto* delta = pop(builder.getInt32Ty());
               push(callback<optimizing::grow_memory_callback>(offsetof(optimizing::invocation_callbacks, grow_memory),
                                                               {invocation(), delta}));
            } else if (op.is_a<interpret::drop_t>())
               static_cast<void>(pop());
            else if (op.is_a<interpret::nop_t>()) {
            } else if (op.is_a<interpret::unreachable_t>()) {
               trap(builder.getInt32(static_cast<std::uint32_t>(optimizing::trap_reason::unreachable)));
            } else if (op.is_a<interpret::call_t>())
               direct_call(op.get<interpret::call_t>().index);
            else if (op.is_a<interpret::call_indirect_t>())
               indirect_call(op.get<interpret::call_indirect_t>().index);
            else if (op.is_a<interpret::return_t>())
               epilogue(type);
            else if (op.is_a<interpret::select_t>()) {
               auto* condition = pop(builder.getInt32Ty());
               auto* right = pop();
               auto* left = pop();
               push(builder.CreateSelect(builder.CreateICmpNE(condition, builder.getInt32(0)), left, right));
            } else if (op.is_a<interpret::br_t>()) {
               const auto& branch = op.get<interpret::br_t>();
               builder.CreateBr(edge(flow, blocks, pc, branch.pc, branch.data));
            } else if (op.is_a<interpret::br_if_t>()) {
               auto* condition = pop(builder.getInt32Ty());
               const auto& branch = op.get<interpret::br_if_t>();
               auto* taken = edge(flow, blocks, pc, branch.pc, branch.data);
               builder.CreateCondBr(builder.CreateICmpNE(condition, builder.getInt32(0)), taken,
                                    blocks[flow.nodes[pc].next]);
            } else if (op.is_a<interpret::if_t>()) {
               auto* condition = pop(builder.getInt32Ty());
               auto* otherwise = edge(flow, blocks, pc, op.get<interpret::if_t>().pc);
               builder.CreateCondBr(builder.CreateICmpNE(condition, builder.getInt32(0)), blocks[flow.nodes[pc].next],
                                    otherwise);
            } else if (op.is_a<interpret::else_t>())
               builder.CreateBr(edge(flow, blocks, pc, op.get<interpret::else_t>().pc));
            else if (op.is_a<interpret::br_table_t>()) {
               auto* selector = pop(builder.getInt32Ty());
               const auto& table = op.get<interpret::br_table_t>();
               auto* otherwise = edge(flow, blocks, pc, table.table[table.size].pc, table.table[table.size].stack_pop);
               auto* instruction = builder.CreateSwitch(selector, otherwise, table.size);
               for (std::uint32_t index = 0; index < table.size; ++index)
                  instruction->addCase(builder.getInt32(index),
                                       edge(flow, blocks, pc, table.table[index].pc, table.table[index].stack_pop));
            } else if (const auto arity = control_flow::numeric_arity(static_cast<interpret::opcodes>(op.index())))
               numeric(static_cast<interpret::opcodes>(op.index()), arity);
            else
               integer(static_cast<interpret::opcodes>(op.index()));
            if (!builder.GetInsertBlock()->getTerminator()) {
               const auto next = flow.nodes[pc].next;
               if (next >= body.size || !blocks[next] || flow.nodes[next].height != height)
                  FORGE_THROW_EXCEPTION(optimizing::exceptions::invalid_module, "typed lowering fallthrough mismatch");
               builder.CreateBr(blocks[next]);
            }
         }
      }
   }
};
} // namespace

std::vector<std::uint8_t> lowering::emit(const interpret::module& parsed,
                                      const optimizing::artifact_payload& metadata,
                                      std::span<const std::uint32_t> canonical_types,
                                      std::string* optimized_ir) {
   const auto& configuration = metadata.configuration;
   static auto targets = std::once_flag{};
   std::call_once(targets, [] {
      llvm::InitializeAllTargetInfos();
      llvm::InitializeAllTargets();
      llvm::InitializeAllTargetMCs();
      llvm::InitializeAllAsmPrinters();
   });
   if (configuration.target.llvm_version != LLVM_VERSION_STRING)
      FORGE_THROW_EXCEPTION(optimizing::exceptions::unsupported_target, "LLVM compiler version does not match request");
   auto error = std::string{};
   const auto triple = llvm::Triple{configuration.target.triple};
   auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
   if (!target)
      FORGE_THROW_EXCEPTION(optimizing::exceptions::unsupported_target, "LLVM compiler target is unavailable");
   auto options = llvm::TargetOptions{};
   options.EmitStackSizeSection = true;
   auto machine = std::unique_ptr<llvm::TargetMachine>{
       target->createTargetMachine(triple, configuration.target.cpu, configuration.target.features, options,
                                   llvm::Reloc::PIC_, llvm::CodeModel::Small, llvm::CodeGenOptLevel::Default)};
   if (!machine || machine->createDataLayout().getPointerSize() != sizeof(void*) || sizeof(void*) != 8 ||
       std::endian::native != std::endian::little || !machine->createDataLayout().isLittleEndian())
      FORGE_THROW_EXCEPTION(optimizing::exceptions::unsupported_target,
                            "LLVM target does not match the 64-bit invocation ABI");
   auto generated = emitter{parsed, metadata, canonical_types};
   generated.output.setTargetTriple(triple);
   generated.output.setDataLayout(machine->createDataLayout());
   generated.lower();
   auto diagnostics = llvm::raw_null_ostream{};
   if (llvm::verifyModule(generated.output, &diagnostics))
      FORGE_THROW_EXCEPTION(optimizing::exceptions::compiler_unavailable, "LLVM verification rejected lowering");
   auto loops = llvm::LoopAnalysisManager{};
   auto functions = llvm::FunctionAnalysisManager{};
   auto graph = llvm::CGSCCAnalysisManager{};
   auto modules = llvm::ModuleAnalysisManager{};
   auto passes = llvm::PassBuilder{machine.get()};
   passes.registerModuleAnalyses(modules);
   passes.registerCGSCCAnalyses(graph);
   passes.registerFunctionAnalyses(functions);
   passes.registerLoopAnalyses(loops);
   passes.crossRegisterProxies(loops, functions, graph, modules);
   passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2).run(generated.output, modules);
   if (llvm::verifyModule(generated.output, &diagnostics))
      FORGE_THROW_EXCEPTION(optimizing::exceptions::compiler_unavailable,
                            "LLVM verification rejected optimized lowering");
   // Private owning-module oracle, not a public compiler API or env-controlled
   // production dump. SAME O2-verified module; null incurs no formatting work.
   if (optimized_ir) {
      optimized_ir->clear();
      auto text = llvm::raw_string_ostream{*optimized_ir};
      generated.output.print(text, nullptr);
   }
   auto bytes = llvm::SmallVector<char, 0>{};
   auto object = llvm::raw_svector_ostream{bytes};
   auto emission = llvm::legacy::PassManager{};
   if (machine->addPassesToEmitFile(emission, object, nullptr, llvm::CodeGenFileType::ObjectFile))
      FORGE_THROW_EXCEPTION(optimizing::exceptions::unsupported_target, "LLVM target cannot emit a native object");
   emission.run(generated.output);
   return {bytes.begin(), bytes.end()};
}

} // namespace forge::tooling::wasm_compiler
