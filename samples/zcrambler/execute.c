#include <stdio.h>
#include <stdlib.h>

#include "add_w2c.h"

int main(int argc, char **argv) {
  /* Make sure there is at least one command-line argument. */
  if (argc < 3) {
    printf("Invalid argument. Expected '%s NUMBER'\n", argv[0]);
    return 1;
  }

  /* Convert the argument from a string to an int. We'll implicitly cast the int
  to a `u32`, which is what `fac` expects. */
  u32 x = atoi(argv[1]);
  u32 y = atoi(argv[2]);


  /* Initialize the Wasm runtime. */
  wasm_rt_init();

  /* Declare an instance of the `fac` module. */
  w2c_squanchy fac;

  /* Construct the module instance. */
  // wasm2c_squanchy_instantiate(&fac);

  /* Call `fac`, using the mangled name. */
  u32 result = (int)w2c_squanchy_add_0(&fac, x, y);

  /* Print the result. */
  printf("calc(%d, %d) -> %d\n", x, y, result);

  /* Free the fac module. */
  //wasm2c_squanchy_free(&fac);

  /* Free the Wasm runtime state. */
  //wasm_rt_free();

  return 0;
}
