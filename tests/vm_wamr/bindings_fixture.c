#include <stdint.h>

__attribute__((import_module("binding"), import_name("mix")))
double mix(uint32_t, double, uint64_t, float);
__attribute__((import_module("binding"), import_name("visit")))
uint32_t visit(uint32_t, uint32_t);

double scalar_call(void) { return mix(7, 2.5, UINT64_C(0x100000003), 1.25f); }
uint32_t range_call(uint32_t offset, uint32_t size) { return visit(offset, size); }
uint32_t run(void) { return visit(0, 0); }
#ifndef FORGE_STATIC_BINDINGS
uint32_t grow(uint32_t pages) { return __builtin_wasm_memory_grow(0, pages); }
uint32_t pages(void) { return __builtin_wasm_memory_size(0); }
#endif
uint32_t load8(uint32_t offset) { return *(volatile unsigned char*)(uintptr_t)offset; }
