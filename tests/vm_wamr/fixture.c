typedef unsigned int u32;
typedef unsigned long long u64;

__attribute__((import_module("probe"), import_name("sum")))
extern u32 host_sum(u32 offset, u32 length);
__attribute__((import_module("probe"), import_name("entered")))
extern void host_entered(void);
__attribute__((import_module("probe"), import_name("poison_fp")))
extern void host_poison_fp(void);
__attribute__((import_module("probe"), import_name("nested_fp")))
extern u32 host_nested_fp(void);

static u32 counter;
static const unsigned char payload[] = {3, 5, 7, 11};

u32 add(u32 a, u32 b) { return a + b; }
u64 wide(u64 a, u64 b) { return a ^ b; }
float native_f32(u32 mark, float a) { return a + mark; }
double native_f64(u32 mark, double a) { return a + mark; }
double native_spill(u32 mark, double a, double b, double c, double d,
                    double e, double f, double g, double h, double i) {
   return mark + a + b + c + d + e + f + g + h + i;
}
u32 increment(void) { return ++counter; }
u32 pages(void) { return __builtin_wasm_memory_size(0); }
u32 grow(u32 count) { return __builtin_wasm_memory_grow(0, count); }
u32 divide(u32 a, u32 b) { return a / b; }
u32 read_byte(u32 offset) { return *(volatile unsigned char *)(unsigned long)offset; }
u32 imported(void) { return host_sum((u32)(unsigned long)payload, sizeof(payload)); }
u32 nested_boundary(void) { return host_nested_fp(); }
u32 imported_range(u32 offset, u32 length) { return host_sum(offset, length); }

u64 add64_bits(u64 a, u64 b) {
   union { u64 bits; double value; } x = {.bits = a}, y = {.bits = b}, result;
   result.value = x.value + y.value;
   return result.bits;
}

u32 add32_bits(u32 a, u32 b) {
   union { u32 bits; float value; } x = {.bits = a}, y = {.bits = b}, result;
   result.value = x.value + y.value;
   return result.bits;
}

u64 nearest64_bits(u64 input) {
   union { u64 bits; double value; } x = {.bits = input}, result;
   result.value = __builtin_nearbyint(x.value);
   return result.bits;
}

void spin(void) {
   host_entered();
   for (;;) {
      counter++;
      __asm__ volatile("" : : : "memory");
   }
}

u64 after_host(u64 value) {
   host_poison_fp();
   return nearest64_bits(value);
}

#define DEFINE_FP(width, type, integer, suffix) \
integer fp##width(u32 op, integer a, integer b) { \
   union { integer bits; type value; } x = {.bits = a}, y = {.bits = b}, r; \
   switch (op) { \
      case 0: r.value = x.value + y.value; break; \
      case 1: r.value = x.value - y.value; break; \
      case 2: r.value = x.value * y.value; break; \
      case 3: r.value = x.value / y.value; break; \
      case 4: r.value = __builtin_sqrt##suffix(x.value); break; \
      case 5: r.value = __builtin_ceil##suffix(x.value); break; \
      case 6: r.value = __builtin_floor##suffix(x.value); break; \
      case 7: r.value = __builtin_trunc##suffix(x.value); break; \
      case 8: r.value = __builtin_nearbyint##suffix(x.value); break; \
      case 9: r.value = __builtin_wasm_min_f##width(x.value, y.value); break; \
      case 10: r.value = __builtin_wasm_max_f##width(x.value, y.value); break; \
      case 11: r.value = __builtin_fabs##suffix(x.value); break; \
      case 12: r.value = -x.value; break; \
      case 13: r.value = __builtin_copysign##suffix(x.value, y.value); break; \
      default: __builtin_trap(); \
   } \
   return r.bits; \
}

DEFINE_FP(32, float, u32, f)
DEFINE_FP(64, double, u64, )

u32 demote(u64 bits) {
   union { u64 bits; double value; } input = {.bits = bits};
   union { u32 bits; float value; } output = {.value = (float)input.value};
   return output.bits;
}

u64 promote(u32 bits) {
   union { u32 bits; float value; } input = {.bits = bits};
   union { u64 bits; double value; } output = {.value = (double)input.value};
   return output.bits;
}
