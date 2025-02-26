#!/bin/sh

clang add.c -target wasm32 -O3 -o add.wasm --no-standard-libraries -Wl,--export-all -Wl,--no-entry -o add_O3.wasm
clang add.c -target wasm32 -O0 -o add.wasm --no-standard-libraries -Wl,--export-all -Wl,--no-entry -o add_O0.wasm

wasm2c add_O0.wasm -o add_O0_w2c.c -n squanchy
wasm2c add_O3.wasm -o add_O3_w2c.c -n squanchy

clang add_O0_w2c.c -S -emit-llvm -O0
clang add_O3_w2c.c -S -emit-llvm -O0


/home/adam/studies/wasm/Squanchy/build/squanchy add_O0_w2c.ll -f w2c_squanchy_add_0 -runtime-path=/home/adam/studies/wasm/Squanchy/build/wasm_runtime.bc --replace-instance-refs=true --inject-initializer=false -o add_O0_w2c_deobf.ll
clang add_O0_w2c_deobf.ll -c

# open  with ida pro
read -p "Press enter to open IDA Pro"

~/ida-pro-9.0/ida -A add_O0_w2c_deobf.o
rm add_O0_w2c_deobf.o.i64

# wait until user press enter
read -p "Press enter to open IDA Pro"

/home/adam/studies/wasm/Squanchy/build/squanchy add_O0_w2c.ll -f w2c_squanchy_add_0 -runtime-path=/home/adam/studies/wasm/Squanchy/build/wasm_runtime.bc --replace-instance-refs=true --inject-initializer=true -o add_O0_w2c_deobf.ll
clang add_O0_w2c_deobf.ll -c

rm add_O0_w2c_deobf.o.i64
~/ida-pro-9.0/ida -A add_O0_w2c_deobf.o

rm add_O0_w2c_deobf.o.i64
