#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm_runtime_common.h"
#include "fp_status_layout.h"
#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

#pragma STDC FENV_ACCESS ON

static unsigned assertions;
static unsigned cases;
static unsigned fail_get, fail_default, fail_round, fail_restore;
static unsigned successful_restores, failed_restores, host_calls;
static bool last_restore_succeeded;
static unsigned bridge_mode;
static bool nested_indirect;
static WASMModuleInstanceCommon* instance;
static WASMFunctionInstanceCommon* leaf;

#define CHECK(expr)                                                                                                    \
   do {                                                                                                                \
      ++assertions;                                                                                                    \
      if (!(expr)) {                                                                                                   \
         fprintf(stderr, "FAIL line=%u case=%u: %s\n", __LINE__, cases, #expr);                                        \
         exit(1);                                                                                                      \
      }                                                                                                                \
   } while (0)

static bool consume(unsigned* remaining) {
   return *remaining && --*remaining == 0;
}

/* Only the separately compiled common-runtime object renames fenv calls.
   A restore fault reports failure AFTER real restoration so the probe itself
   does not leave the runner's hardware FP state corrupted. */
int probe_fegetenv(fenv_t* env) {
   if (consume(&fail_get))
      return -1;
   return fegetenv(env);
}

int probe_fesetenv(const fenv_t* env) {
   int result = fesetenv(env);
   if (env == FE_DFL_ENV) {
      if (consume(&fail_default))
         return -1;
   } else {
      if (consume(&fail_restore)) {
         last_restore_succeeded = false;
         ++failed_restores;
         return -1;
      }
      last_restore_succeeded = result == 0;
      ++successful_restores;
   }
   return result;
}

int probe_fesetround(int mode) {
   int result = fesetround(mode);
   return consume(&fail_round) ? -1 : result;
}

typedef struct FPState {
   int rounding;
   int flags;
#if defined(__aarch64__) || defined(__arm64__)
   uint64_t control;
   uint64_t status;
#else
   unsigned control;
#endif
} FPState;

static FPState fp_state(void) {
   FPState result = {0};
   result.rounding = fegetround();
   result.flags = fetestexcept(FE_ALL_EXCEPT);
#if defined(__aarch64__) || defined(__arm64__)
   __asm__ volatile("mrs %0, fpcr" : "=r"(result.control));
   __asm__ volatile("mrs %0, fpsr" : "=r"(result.status));
#else
   result.control = _mm_getcsr();
#endif
   return result;
}

static void check_fp_state(FPState expected) {
   FPState actual = fp_state();
   CHECK(actual.rounding == expected.rounding);
   CHECK(actual.flags == expected.flags);
   CHECK(actual.control == expected.control);
#if defined(__aarch64__) || defined(__arm64__)
   CHECK(actual.status == expected.status);
#endif
}

static void begin_case(WASMExecEnv* env, const char* name) {
   ++cases;
   fail_get = fail_default = fail_round = fail_restore = 0;
   successful_restores = failed_restores = host_calls = 0;
   last_restore_succeeded = false;
   bridge_mode = 0;
   nested_indirect = false;
   CHECK(fesetenv(FE_DFL_ENV) == 0);
   CHECK(fesetround(FE_UPWARD) == 0);
   CHECK(feraiseexcept(FE_INVALID | FE_INEXACT) == 0);
   /* Embedder reset, only outside any common-runtime FP guard. */
   env->forge_fp_failure = WASM_FP_FAILURE_NONE;
   wasm_runtime_clear_exception(wasm_runtime_get_module_inst(env));
   printf("CASE %u %s\n", cases, name);
}

static bool nested_call(WASMExecEnv* env, uint32_t cells[2]) {
   return nested_indirect ? wasm_runtime_call_indirect(env, 0, 0, cells) : wasm_runtime_call_wasm(env, leaf, 0, cells);
}

static uint32_t bridge(WASMExecEnv* env) {
   uint32_t cells[2] = {0, 0};
   ++host_calls;
   CHECK(fegetround() == FE_TONEAREST);
   if (!bridge_mode)
      return 123;
   CHECK(fesetround(FE_DOWNWARD) == 0);
   if (bridge_mode == 2 || bridge_mode == 4)
      fail_get = 1;
   if (bridge_mode == 3)
      fail_restore = 1;
   bool result = nested_call(env, cells);
   if (bridge_mode == 1) {
      CHECK(result);
      CHECK(cells[0] == UINT32_C(0x3f800000));
      CHECK(env->forge_fp_failure == WASM_FP_FAILURE_NONE);
   } else {
      WASMFPFailure expected = bridge_mode == 3 ? WASM_FP_FAILURE_RESTORE_FAILED : WASM_FP_FAILURE_ENTER_FAILED;
      CHECK(!result);
      CHECK(env->forge_fp_failure == expected);
      if (bridge_mode == 4) {
         /* A later restore failure must not replace the first enter error. */
         fail_restore = 1;
         CHECK(!nested_call(env, cells));
         CHECK(env->forge_fp_failure == expected);
      }
      /* A successful nested guard and outer native restore must not clear it. */
      CHECK(nested_call(env, cells));
      CHECK(cells[0] == UINT32_C(0x3f800000));
      CHECK(env->forge_fp_failure == expected);
   }
   CHECK(fegetround() == FE_DOWNWARD);
   return cells[0];
}

static void bridge_raw(WASMExecEnv* env, uint64_t* args) {
   args[0] = bridge(env);
}

static const uint8_t wasm[] = {
    0,   0x61, 0x73, 0x6d, 1,   0,   0,    0,    1,    5,    1,    0x60, 0,  1,    0x7f, 2,   14,  1,    3,
    'e', 'n',  'v',  6,    'b', 'r', 'i',  'd',  'g',  'e',  0,    0,    3,  3,    2,    0,   0,   4,    4,
    1,   0x70, 0,    1,    7,   16,  2,    4,    'l',  'e',  'a',  'f',  0,  1,    5,    'o', 'u', 't',  'e',
    'r', 0,    2,    9,    7,   1,   0,    0x41, 0,    0x0b, 1,    1,    10, 21,   2,    14,  0,   0x43, 0,
    0,   0x80, 0x3f, 0x43, 0,   0,   0x80, 0x33, 0x92, 0xbc, 0x0b, 4,    0,  0x10, 0,    0x0b};

int main(void) {
   fenv_t original;
   RuntimeInitArgs init = {0};
   printf("WAMR runtime identity: %s\n", FORGE_FP_STATUS_RUNTIME_IDENTITY);
   CHECK(fegetenv(&original) == 0);
   init.mem_alloc_type = Alloc_With_System_Allocator;
   init.running_mode = Mode_Interp;
   CHECK(wasm_runtime_full_init(&init));
   for (unsigned raw = 0; raw != 2; ++raw) {
      NativeSymbol symbol = {"bridge", raw ? (void*)bridge_raw : (void*)bridge, "()i", NULL};
      CHECK(raw ? wasm_runtime_register_natives_raw("env", &symbol, 1)
                : wasm_runtime_register_natives("env", &symbol, 1));
      char diagnostic[512] = {0};
      uint8_t code[sizeof(wasm)];
      memcpy(code, wasm, sizeof(wasm));
      WASMModuleCommon* module = wasm_runtime_load(code, sizeof(code), diagnostic, sizeof(diagnostic));
      if (!module)
         fprintf(stderr, "Load diagnostic: %s\n", diagnostic);
      CHECK(module);
      struct InstantiationArgs2* args;
      CHECK(wasm_runtime_instantiation_args_create(&args));
      wasm_runtime_instantiation_args_set_default_stack_size(args, 16384);
      wasm_runtime_instantiation_args_set_defer_start(args, true);
      instance = wasm_runtime_instantiate_ex2(module, args, diagnostic, sizeof(diagnostic));
      CHECK(instance);
      WASMExecEnv* env = wasm_runtime_create_exec_env(instance, 16384);
      CHECK(env);
      CHECK(env->forge_fp_failure == WASM_FP_FAILURE_NONE);
      CHECK(wasm_runtime_initialize_with_exec_env(instance, env));
      leaf = wasm_runtime_lookup_function(instance, "leaf");
      WASMFunctionInstanceCommon* outer = wasm_runtime_lookup_function(instance, "outer");
      CHECK(leaf && outer);
      for (unsigned operation = 0; operation != 4; ++operation) {
         for (unsigned fault = 0; fault != 6; ++fault) {
            WASMModuleInstanceCommon* target = instance;
            WASMExecEnv* target_env = env;
            if (operation == 2) {
               target = wasm_runtime_instantiate_ex2(module, args, diagnostic, sizeof(diagnostic));
               CHECK(target);
               target_env = wasm_runtime_create_exec_env(target, 16384);
               CHECK(target_env);
            }
            begin_case(target_env, raw ? "raw-profile outer guard" : "native-profile outer guard");
            printf("  operation=%u fault=%u\n", operation, fault);
            FPState expected = fp_state();
            if (fault == 1)
               fail_get = 1;
            if (fault == 2)
               fail_default = 1;
            if (fault == 3)
               fail_round = 1;
            if (fault == 4)
               fail_restore = 1;
            if (fault == 5) {
               /* Setup error wins even when its rollback also fails. */
               fail_round = fail_restore = 1;
            }
            bool result;
            uint32_t cells[2] = {0, 0};
            if (operation == 0)
               result = wasm_runtime_call_wasm(env, leaf, 0, cells);
            else if (operation == 1)
               result = wasm_runtime_call_indirect(env, 0, 0, cells);
            else if (operation == 2)
               result = wasm_runtime_initialize_with_exec_env(target, target_env);
            else {
               WASMModuleInstanceCommon* created =
                   wasm_runtime_instantiate_internal(module, NULL, env, args, diagnostic, sizeof(diagnostic));
               result = created != NULL;
               if (created)
                  wasm_runtime_deinstantiate(created);
            }
            CHECK(result == (fault == 0));
            CHECK(target_env->forge_fp_failure == (fault == 0   ? WASM_FP_FAILURE_NONE
                                                   : fault == 4 ? WASM_FP_FAILURE_RESTORE_FAILED
                                                                : WASM_FP_FAILURE_ENTER_FAILED));
            CHECK(host_calls == 0);
            if (fault == 5)
               CHECK(failed_restores == 1);
            check_fp_state(expected);
            if (operation == 2) {
               wasm_runtime_destroy_exec_env(target_env);
               wasm_runtime_deinstantiate(target);
            }
         }
      }
      for (unsigned fault = 1; fault != 3; ++fault) {
         begin_case(env, raw ? "raw bridge guard failure" : "native bridge guard failure");
         FPState expected = fp_state();
         if (fault == 1)
            fail_get = 2;
         else
            fail_restore = 1;
         uint32_t cells[2] = {0, 0};
         (void)wasm_runtime_call_wasm(env, outer, 0, cells);
         CHECK(env->forge_fp_failure == (fault == 1 ? WASM_FP_FAILURE_ENTER_FAILED : WASM_FP_FAILURE_RESTORE_FAILED));
         CHECK(host_calls == (fault == 1 ? 0U : 1U));
         CHECK(successful_restores >= 1);
         CHECK(last_restore_succeeded);
         CHECK(failed_restores == (fault == 1 ? 0U : 1U));
         check_fp_state(expected);
      }
      for (unsigned indirect = 0; indirect != 2; ++indirect) {
         for (unsigned mode = 1; mode != 5; ++mode) {
            begin_case(env, raw ? "raw nested bridge sticky status" : "native nested bridge sticky status");
            FPState expected = fp_state();
            nested_indirect = indirect != 0;
            bridge_mode = mode;
            uint32_t cells[2] = {0, 0};
            CHECK(wasm_runtime_call_wasm(env, outer, 0, cells));
            CHECK(cells[0] == UINT32_C(0x3f800000));
            CHECK(env->forge_fp_failure == (mode == 1   ? WASM_FP_FAILURE_NONE
                                            : mode == 3 ? WASM_FP_FAILURE_RESTORE_FAILED
                                                        : WASM_FP_FAILURE_ENTER_FAILED));
            CHECK(host_calls == 1);
            CHECK(successful_restores >= 3);
            CHECK(last_restore_succeeded);
            CHECK(failed_restores == (mode < 3 ? 0U : 1U));
            check_fp_state(expected);
         }
      }
      wasm_runtime_destroy_exec_env(env);
      wasm_runtime_deinstantiate(instance);
      wasm_runtime_instantiation_args_destroy(args);
      wasm_runtime_unload(module);
      CHECK(wasm_runtime_unregister_natives("env", &symbol));
   }
   CHECK(cases == 68);
   wasm_runtime_destroy();
   CHECK(fesetenv(&original) == 0);
   printf("PASS cases=%u assertions=%u\n", cases, assertions);
   return 0;
}
