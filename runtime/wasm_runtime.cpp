/*
    Some helpers needed to unfold the lifted wasm2c code and let it fold
*/
#include <cstdint>

// The wasm runtime header.
#include "wasm-rt.h"

// The default page size is 64KB.
#define PAGE_SIZE 65536

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

}; // extern "C"