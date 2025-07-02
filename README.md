# Squanchy

Squanchy is a tool to deobfuscate obfuscated WebAssembly (WASM) code. It leverages a combination of existing tools and powerful LLVM optimizations to analyze and restore readable code from heavily obfuscated WASM binaries.

## Features

- Deobfuscates WASM code for improved readability and analysis
- Utilizes advanced LLVM optimization passes
- Integrates techniques from multiple deobfuscation tools

## Usage

1. Install Squanchy (see [Installation](#installation))
2. Run Squanchy on your obfuscated WASM file:
    ```squanchy add_O0_w2c.ll -f w2c_squanchy_add_0 -runtime-path=wasm_runtime.bc --replace-instance-refs=true --inject-initializer=true
    ```

## Installation

Instructions coming soon.
