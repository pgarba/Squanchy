/*
    Some helpers needed to unfold the lifted wasm2c code and let it fold
*/
#include <cstdint>

// The wasm runtime header.
#include "wasm-rt.h"

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

/*
    Free memory for the wasm runtime.
*/
void __attribute__((always_inline))
wasm_rt_free_memory(wasm_rt_memory_t *memory) {
  free(memory->data);
  memory->data = nullptr;
}

/*
    Grow memory for the wasm runtime.
*/
bool __attribute__((always_inline)) wasm_rt_is_initialized() { return true; }

}; // extern "C"