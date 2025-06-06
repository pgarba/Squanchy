/*
    Some helpers needed to unfold the lifted wasm2c code and let it fold
*/
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// The wasm runtime header.
#include "wasm-rt.h"

// #define USE_RAM

// The default page size is 64KB.
#define PAGE_SIZE 65536

/*
  // emcc generated values
  instance->w2c_0x5F_dso_handle = 1024u;
  instance->w2c_0x5F_data_end = 1044u;
  instance->w2c_0x5F_stack_low = 1056u;
  instance->w2c_0x5F_stack_high = 66592u;
  instance->w2c_0x5F_global_base = 1024u;
  instance->w2c_0x5F_heap_base = 66592u;
  instance->w2c_0x5F_heap_end = 131072u;
  instance->w2c_0x5F_memory_base = 0u;
  instance->w2c_0x5F_table_base = 1u;
*/

extern "C" {

// Memory
extern "C" uint8_t FUNCREF_TABLE[0];
extern "C" uint8_t EXTERNREF_TABLE[0];
extern "C" uint8_t MEMORY[0];
extern "C" uint8_t DYNAMICTOP_PTR[0];
extern "C" uint8_t STACKTOP[0];
extern "C" uint8_t STACK_MAX[0];
extern "C" uint8_t MEMORY_PAGES[0];

/*
    Allocate memory for the wasm runtime.
*/
void __attribute__((always_inline))
wasm_rt_allocate_memory(wasm_rt_memory_t *memory, uint64_t initial_pages,
                        uint64_t max_pages, bool is64) {
  memory->pages = initial_pages;
  memory->max_pages = max_pages;
  memory->size = initial_pages * PAGE_SIZE;

  // #ifdef USE_RAM
  memory->data = (uint8_t *)calloc(memory->size, 1);
  //  #else
  //    memory->data = (uint8_t *)MEMORY_PAGES;
  //  #endif

  memory->is64 = is64;
}

/*
    Free memory for the wasm runtime.
*/
void __attribute__((always_inline))
wasm_rt_free_memory(wasm_rt_memory_t *memory) {
#ifdef USE_RAM
  free(memory->data);
#endif
  memory->data = nullptr;
}

/*
    Grow memory for the wasm runtime.
*/
bool __attribute__((always_inline)) wasm_rt_is_initialized() { return true; }

/*
    Allocate funcref table for the wasm runtime.
*/
void __attribute__((always_inline))
wasm_rt_allocate_funcref_table(wasm_rt_funcref_table_t *table,
                               uint32_t elements, uint32_t max_elements) {
  table->size = elements;
  table->max_size = max_elements;
  // #ifdef USE_RAM
  // table->data =
  //    (wasm_rt_funcref_t *)calloc(table->size, sizeof(wasm_rt_funcref_t));
  // #else
  table->data = (wasm_rt_funcref_t *)FUNCREF_TABLE;
  // #endif
}

/*
    Allocate externref table for the wasm runtime.
*/
void __attribute__((always_inline))
wasm_rt_allocate_externref_table(wasm_rt_externref_table_t *table,
                                 uint32_t elements, uint32_t max_elements) {
  table->size = elements;
  table->max_size = max_elements;
  // #ifdef USE_RAM
  //   table->data =
  //       (wasm_rt_externref_t *)calloc(table->size,
  //       sizeof(wasm_rt_externref_t));
  // #else
  table->data = (wasm_rt_externref_t *)EXTERNREF_TABLE;
  // #endif
}

/*
  Implement some helpers for wasm env
*/
const uint32_t memoryBase = 0;
const uint32_t tableBase = 1;

/*
  wasm2c generated values example:

  const u64 wasm2c_squanchy_min_env_memory = 4096;
  const u64 wasm2c_squanchy_max_env_memory = 4096;
  const u8 wasm2c_squanchy_is64_env_memory = 0;
  const u32 wasm2c_squanchy_min_env_table = 6;
  const u32 wasm2c_squanchy_max_env_table = 6;
*/
extern "C" const uint64_t wasm2c_squanchy_min_env_memory;
extern "C" const uint64_t wasm2c_squanchy_max_env_memory;
extern "C" const uint8_t wasm2c_squanchy_is64_env_memory;
extern "C" const uint32_t wasm2c_squanchy_min_env_table;
extern "C" const uint32_t wasm2c_squanchy_max_env_table;

// Firefox
// TOTAL_STACK: 5242880
#define StackSize 5242880

// Out struct to hold the wasm2c env
struct w2c_env {
  uint32_t *DYNAMICTOP_PTR;

  wasm_rt_memory_t memory;
  uint32_t memoryBase;

  wasm_rt_funcref_table_t funcref_table;
  uint32_t funcref_tableBase;

  // Taken from Firefox
  uint32_t *STACKTOP;

  //  const uint32_t StackSize = 5242880;
};

// Keep it here to keep the type
extern "C" const int w2c_env_size = sizeof(struct w2c_env);

extern "C" uint32_t *__attribute__((always_inline))
w2c_env_DYNAMICTOP_PTR(struct w2c_env *env) {
  return (uint32_t *)env->DYNAMICTOP_PTR;
}

extern "C" void *Stack;
extern "C" uint32_t *__attribute__((always_inline))
w2c_env_STACKTOP(struct w2c_env *env) {
  // #ifdef USE_RAM
  // env->STACKTOP = (uint32_t *)calloc(1, StackSize); // alloca(StackSize);
  // #else
  env->STACKTOP = (uint32_t *)&Stack;

  void **p = (void **)env->STACKTOP;
  Stack = (void *)alloca(10000); // alloca(StackSize);
  //  #endif

  return env->STACKTOP;
}

extern "C" uint32_t *__attribute__((always_inline))
w2c_env_STACK_MAX(struct w2c_env *env) {
  // #ifdef USE_RAM
  //  return &env->STACKTOP[env->StackSize];
  // return (uint32_t *)&StackSize;
  // #else
  return (uint32_t *)STACK_MAX;
  // #endif
}

extern "C" wasm_rt_memory_t *__attribute__((always_inline))
w2c_env_memory(struct w2c_env *env) {
  // Minimum 1 page
  uint64_t initial_pages = wasm2c_squanchy_min_env_memory / PAGE_SIZE;
  uint64_t max_pages = wasm2c_squanchy_max_env_memory / PAGE_SIZE;

  // Check if at least 1 page
  if (initial_pages == 0) {
    initial_pages = 1;
  }

  if (max_pages == 0) {
    max_pages = 1;
  }

  // Allocate and mark as 64 bit
  wasm_rt_allocate_memory(&env->memory, initial_pages, max_pages, true);

  // Set DYNAMICTOP_PTR
  env->DYNAMICTOP_PTR =
      (uint32_t *)DYNAMICTOP_PTR; // (uint32_t
                                  // *)&env->memory.data[env->memory.size];

  return (wasm_rt_memory_t *)&env->memory;
}

extern "C" uint32_t *__attribute__((always_inline))
w2c_env_memoryBase(struct w2c_env *env) {
  env->memoryBase = 0;
  return (uint32_t *)&env->memoryBase;
}

// Todo: allocate this dynamically by parsing the params from funcref_table_init
// call and create an alloca
wasm_rt_funcref_table_t wasm_rt_funcref_table;
extern "C" wasm_rt_funcref_table_t *__attribute__((always_inline))
w2c_env_table(struct w2c_env *env) {
  // Init
  wasm_rt_allocate_funcref_table(&env->funcref_table,
                                 wasm2c_squanchy_min_env_table,
                                 wasm2c_squanchy_max_env_table);

  return &env->funcref_table;
}

extern "C" uint32_t *__attribute__((always_inline))
w2c_env_tableBase(struct w2c_env *env) {
  env->funcref_tableBase = 0;
  return (uint32_t *)&env->funcref_tableBase;
}

}; // extern "C"