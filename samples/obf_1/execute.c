#include <stdio.h>
#include <stdlib.h>

#include "obf_w2c.h"

int calc_org(unsigned int n) {

  unsigned int mod = n % 4;

  unsigned int result = 0;

  if (mod == 0)
    result = (n | 0xBAAAD0BF) * (2 ^ n);

  else if (mod == 1)
    result = (n & 0xBAAAD0BF) * (3 + n);

  else if (mod == 2)
    result = (n ^ 0xBAAAD0BF) * (4 | n);

  else
    result = (n + 0xBAAAD0BF) * (5 & n);

  return result;
}

int main(int argc, char **argv) {
  /* Make sure there is at least one command-line argument. */
  if (argc < 2) {
    printf("Invalid argument. Expected '%s NUMBER'\n", argv[0]);
    return 1;
  }

  /* Convert the argument from a string to an int. We'll implicitly cast the int
  to a `u32`, which is what `fac` expects. */
  u32 x = atoi(argv[1]);

  /* Initialize the Wasm runtime. */
  wasm_rt_init();

  /* Declare an instance of the `fac` module. */
  w2c_squanchy fac;
  void *wasi, *wasi_env;

  /* Construct the module instance. */
  wasm2c_squanchy_instantiate(&fac, wasi_env, wasi);

  /* Call `fac`, using the mangled name. */
  u32 result = (int)w2c_squanchy_calc_0(&fac, x);
  int result2 = calc_org(x);

  /* Print the result. */
  printf("calc(%X) -> %X , %X\n", x, result, result2);

  /* Free the fac module. */
  wasm2c_squanchy_free(&fac);

  /* Free the Wasm runtime state. */
  wasm_rt_free();

  return 0;
}