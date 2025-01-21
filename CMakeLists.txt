cmake_minimum_required(VERSION 3.16.0)
project(squanchy)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "LLVM_TOOLS ${LLVM_TOOLS_BINARY_DIR}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Set your project compile flags.
execute_process(COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-config --libs OUTPUT_VARIABLE LLVM_LIBS)
execute_process(COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-config --system-libs OUTPUT_VARIABLE SYS_LIBS)
execute_process(COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-config --ldflags OUTPUT_VARIABLE LDF)

execute_process(COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-config --cxxflags OUTPUT_VARIABLE CMAKE_CXX_FLAGS)
string(STRIP ${CMAKE_CXX_FLAGS} CMAKE_CXX_FLAGS)

string(STRIP ${LLVM_LIBS} LLVM_LIBS)
string(STRIP ${SYS_LIBS} SYS_LIBS)
string(STRIP ${LDF} LDF)

# split LLVM_LIBS into multiple lines
string(REPLACE " " ";" LLVM_LIBS ${LLVM_LIBS})

# split SYS_LIBS into multiple lines
string(REPLACE " " ";" SYS_LIBS ${SYS_LIBS})

# split LDF into multiple lines
string(REPLACE " " ";" LDF ${LDF})

set(LLVM_LIBS ${LLVM_LIBS} ${SYS_LIBS})
set(LLVM_LIBS ${LLVM_LIBS} ${LDF})

# add llvm at end so all linkers can find it
list(APPEND PROJECT_LIBRARIES ${LLVM_LIBS})
list(APPEND PROJECT_DEFINITIONS ${LLVM_DEFINITIONS})
list(APPEND PROJECT_INCLUDEDIRECTORIES ${LLVM_INCLUDE_DIRS})

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS} 
)

# Now build our tools
add_executable(squanchy 
src/Squanchy.cpp 
src/Deobfuscator.cpp 
src/LLVMHelpers.cpp
src/LLVMExtract.cpp
)

# Find the libraries that correspond to the LLVM components
# that we wish to use
# llvm_map_components_to_libnames(llvm_libs -19)

# Link against LLVM libraries
target_link_libraries(squanchy ${LLVM_LIBS})

# Custom buld step to compule the runtime
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/runtime.bc
    COMMAND clang++ -c -emit-llvm -o ${CMAKE_CURRENT_BINARY_DIR}/wasm_runtime.bc ${CMAKE_CURRENT_SOURCE_DIR}/runtime/wasm_runtime.cpp
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/runtime/wasm_runtime.cpp
    COMMENT "Building wasm_runtime.bc"
)

add_custom_target(runtime ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/runtime.bc)
