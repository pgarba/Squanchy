/*
    Helpers to unfold the lifted wasm2c code and deobfuscate it.
*/
#include <cstdint>

#include "wasm-rt.h"

#define PAGE_SIZE 65536

extern "C" {

extern void init_globals(void *w2cInstance);
extern void init_memories(void *w2cInstance);
extern void init_data_instances(void *w2cInstance);

/*
    Allocate memory for the wasm runtime.
*/
void __attribute__((always_inline))
wasm_rt_allocate_memory(wasm_rt_memory_t *memory, uint64_t initial_pages,
                        uint64_t max_pages, bool is64) {
  memory->pages = initial_pages;
  memory->max_pages = max_pages;
  memory->size = initial_pages * PAGE_SIZE;
  memory->data = (uint8_t *)calloc(memory->size, 1);
}

void __attribute__((always_inline))
wasm_rt_free_memory(wasm_rt_memory_t *memory) {
  free(memory->data);
}

/*
    Call the wasm2c initializers to initialize the instance.
*/
/*
void *__attribute__((always_inline)) Init(void *w2cInstance) {
  // 1. init_globals
  init_globals(w2cInstance);

  // 2. init_memories
  init_memories(w2cInstance);

  // 3. init_data_instances
  init_data_instances(w2cInstance);

  return w2cInstance;
};
*/
};