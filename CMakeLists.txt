cmake_minimum_required(VERSION 3.16.0)
project(squanchy)

find_package(LLVM REQUIRED CONFIG)

# Add Z3
find_package(Z3 REQUIRED)

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

# z3
message(STATUS "Compiling with Z3 SMT solver")
find_package(Z3 REQUIRED)
message(STATUS "Z3 version: ${Z3_VERSION}")
if(TARGET z3::libz3)
    link_libraries(z3::libz3)
elseif(DEFINED Z3_INCLUDE_DIRS)
    include_directories(${Z3_INCLUDE_DIRS})
else()
    if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
        include_directories(${Z3_INCLUDE_DIRS})
    endif()
    # message(FATAL_ERROR "Unexpected Z3 package search outcome: neither target z3::libz3 not variable Z3_INCLUDE_DIRS exists.")
endif()

# if not defined Z3_CXX_INCLUDE_DIRS
if(NOT DEFINED Z3_CXX_INCLUDE_DIRS)
    if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
        set(Z3_CXX_INCLUDE_DIRS "/usr/include")
    elseif (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        set(Z3_CXX_INCLUDE_DIRS "/opt/homebrew/Cellar/z3/4.13.4/include")
    endif()
endif()

message(STATUS "Z3 Z3_INCLUDE_DIRS: " ${Z3_INCLUDE_DIRS})
message(STATUS "Z3 include directory: " ${Z3_CXX_INCLUDE_DIRS})
message(STATUS "Z3 library: " ${Z3_LIBRARIES})
include_directories(${Z3_CXX_INCLUDE_DIRS} ${Z3_INCLUDE_DIRS} "include")

# Souper
include_directories("/Users/peter/github/saturn16/Saturn/deps/souper/include")
link_directories("/Users/peter/github/saturn16/Saturn/deps/souper/lib")

# Hiredis
include_directories("/opt/homebrew/Cellar/hiredis/1.2.0/include")
link_directories("/opt/homebrew/Cellar/hiredis/1.2.0/lib")

# Alive2
include_directories("/Users/peter/github/saturn16/Saturn/deps/alive2/include")
link_directories("/Users/peter/github/saturn16/Saturn/deps/alive2/lib")

# Now build our tools
add_executable(squanchy 
src/Squanchy.cpp 
src/Deobfuscator.cpp 
src/LLVMHelpers.cpp
src/LLVMExtract.cpp
src/SiMBAPass.cpp
src/SymLLVM.cpp
src/ConstantSynthesis.cpp
src/ConstSynthesis.cpp
src/SolverCache.cpp
src/Solver.cpp
)

# Find the libraries that correspond to the LLVM components
# that we wish to use
# llvm_map_components_to_libnames(llvm_libs -19)
add_subdirectory(dependencies/SiMBA- EXCLUDE_FROM_ALL)

# add SiMBA to the project
include_directories(dependencies/SiMBA-/include)

# Link against LLVM libraries
target_link_libraries(squanchy ${LLVM_LIBS} LSiMBA++ ${Z3_LIBRARIES} hiredis souperExtractor kleeExpr souperCodegen souperInfer souperInst souperKVStore souperParser souperSMTLIB2 ir smt tools util)

# Custom buld step to compule the runtime
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/runtime.bc
    COMMAND clang++ -c -emit-llvm -o ${CMAKE_CURRENT_BINARY_DIR}/wasm_runtime.bc ${CMAKE_CURRENT_SOURCE_DIR}/runtime/wasm_runtime.cpp
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/runtime/wasm_runtime.cpp
    COMMENT "Building wasm_runtime.bc"
)

add_custom_target(runtime ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/runtime.bc)
