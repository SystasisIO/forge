/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 * Immutable layout-only oracle from wasm_exec_env.h at WAMR commit
 * 12d1f906668cde93c03e7d864e71ee1e10dfb194, before the FP status field.
 * No execution or allocation implementation is copied here.
 */
#ifndef FORGE_FP_STATUS_LAYOUT_H
#define FORGE_FP_STATUS_LAYOUT_H

#include <stddef.h>
#include "wasm_runtime_common.h"

typedef struct WASMExecEnvBeforeFPStatus {
   struct WASMExecEnv* next;
   struct WASMInterpFrame* cur_frame;
   struct WASMModuleInstanceCommon* module_inst;
#if WASM_ENABLE_AOT != 0
   uint32* argv_buf;
#endif
   uint8* native_stack_boundary;
   WASMSuspendFlags suspend_flags;
   uintptr_t aux_stack_boundary;
   uintptr_t aux_stack_bottom;
#if WASM_ENABLE_AOT != 0
   void** native_symbol;
#endif
   uint8* native_stack_top_min;
   struct {
      uint8* top_boundary;
      uint8* top;
      uint8* bottom;
   } wasm_stack;
   uint32 forge_call_depth;
   uint32 forge_max_call_depth;
   uint32 forge_depth_exceeded;
#if WASM_ENABLE_INSTRUCTION_METERING != 0
   int instructions_to_execute;
#endif
#if WASM_ENABLE_FAST_JIT != 0
   uint64 jit_cache[2];
#endif
#if WASM_ENABLE_THREAD_MGR != 0
   void* thread_ret_value;
   void* (*thread_start_routine)(void*);
   void* thread_arg;
   WASMCluster* cluster;
   korp_mutex wait_lock;
   korp_cond wait_cond;
   uint32 wait_count;
   bool thread_is_detached;
   bool is_aux_stack_allocated;
#endif
#if WASM_ENABLE_GC != 0
   struct WASMLocalObjectRef* cur_local_object_ref;
#endif
#if WASM_ENABLE_DEBUG_INTERP != 0
   WASMCurrentEnvStatus* current_status;
#endif
   void* attachment;
   void* user_data;
   uint8* user_native_stack_boundary;
   korp_tid handle;
#if WASM_ENABLE_INTERP != 0 && WASM_ENABLE_FAST_INTERP == 0
   BlockAddr block_addr_cache[BLOCK_ADDR_CACHE_SIZE][BLOCK_ADDR_CONFLICT_SIZE];
#endif
#ifdef OS_ENABLE_HW_BOUND_CHECK
   WASMJmpBuf* jmpbuf_stack_top;
   uint8* exce_check_guard_page;
#endif
#if WASM_ENABLE_MEMORY_PROFILING != 0
   uint32 max_wasm_stack_used;
#endif
   uint32 wasm_stack_size;
   union {
      uint64 __make_it_8_byte_aligned_;
      uint8 bottom[1];
   } wasm_stack_u;
} WASMExecEnvBeforeFPStatus;

#define FORGE_FP_OLD_OFFSET(field)                                                                                     \
   _Static_assert(offsetof(WASMExecEnv, field) == offsetof(WASMExecEnvBeforeFPStatus, field),                          \
                  "pre-FP-status offset drift: " #field)

FORGE_FP_OLD_OFFSET(next);
FORGE_FP_OLD_OFFSET(cur_frame);
FORGE_FP_OLD_OFFSET(module_inst);
#if WASM_ENABLE_AOT != 0
FORGE_FP_OLD_OFFSET(argv_buf);
#endif
FORGE_FP_OLD_OFFSET(native_stack_boundary);
FORGE_FP_OLD_OFFSET(suspend_flags);
FORGE_FP_OLD_OFFSET(aux_stack_boundary);
FORGE_FP_OLD_OFFSET(aux_stack_bottom);
#if WASM_ENABLE_AOT != 0
FORGE_FP_OLD_OFFSET(native_symbol);
#endif
FORGE_FP_OLD_OFFSET(native_stack_top_min);
FORGE_FP_OLD_OFFSET(wasm_stack.top_boundary);
FORGE_FP_OLD_OFFSET(wasm_stack.top);
FORGE_FP_OLD_OFFSET(wasm_stack.bottom);
FORGE_FP_OLD_OFFSET(forge_call_depth);
FORGE_FP_OLD_OFFSET(forge_max_call_depth);
FORGE_FP_OLD_OFFSET(forge_depth_exceeded);
#if WASM_ENABLE_INSTRUCTION_METERING != 0
FORGE_FP_OLD_OFFSET(instructions_to_execute);
#endif
#if WASM_ENABLE_FAST_JIT != 0
FORGE_FP_OLD_OFFSET(jit_cache);
#endif
#if WASM_ENABLE_THREAD_MGR != 0
FORGE_FP_OLD_OFFSET(thread_ret_value);
FORGE_FP_OLD_OFFSET(thread_start_routine);
FORGE_FP_OLD_OFFSET(thread_arg);
FORGE_FP_OLD_OFFSET(cluster);
FORGE_FP_OLD_OFFSET(wait_lock);
FORGE_FP_OLD_OFFSET(wait_cond);
FORGE_FP_OLD_OFFSET(wait_count);
FORGE_FP_OLD_OFFSET(thread_is_detached);
FORGE_FP_OLD_OFFSET(is_aux_stack_allocated);
#endif
#if WASM_ENABLE_GC != 0
FORGE_FP_OLD_OFFSET(cur_local_object_ref);
#endif
#if WASM_ENABLE_DEBUG_INTERP != 0
FORGE_FP_OLD_OFFSET(current_status);
#endif
FORGE_FP_OLD_OFFSET(attachment);
FORGE_FP_OLD_OFFSET(user_data);
FORGE_FP_OLD_OFFSET(user_native_stack_boundary);
FORGE_FP_OLD_OFFSET(handle);
#if WASM_ENABLE_INTERP != 0 && WASM_ENABLE_FAST_INTERP == 0
FORGE_FP_OLD_OFFSET(block_addr_cache);
#endif
#ifdef OS_ENABLE_HW_BOUND_CHECK
FORGE_FP_OLD_OFFSET(jmpbuf_stack_top);
FORGE_FP_OLD_OFFSET(exce_check_guard_page);
#endif
#if WASM_ENABLE_MEMORY_PROFILING != 0
FORGE_FP_OLD_OFFSET(max_wasm_stack_used);
#endif
FORGE_FP_OLD_OFFSET(wasm_stack_size);
#undef FORGE_FP_OLD_OFFSET

_Static_assert(WASM_FP_FAILURE_NONE == 0 && WASM_FP_FAILURE_ENTER_FAILED == 1 && WASM_FP_FAILURE_RESTORE_FAILED == 2,
               "FP status contract changed");
_Static_assert(sizeof(WASMFPFailure) == sizeof(uint32), "FP status width changed");
_Static_assert(offsetof(WASMExecEnv, forge_fp_failure) >=
                   offsetof(WASMExecEnvBeforeFPStatus, wasm_stack_size) + sizeof(uint32),
               "FP status overlaps an old field");
_Static_assert(offsetof(WASMExecEnv, wasm_stack_u) >= offsetof(WASMExecEnv, forge_fp_failure) + sizeof(WASMFPFailure),
               "FP status overlaps variable stack storage");

#endif
