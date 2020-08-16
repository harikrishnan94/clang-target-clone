find_package(LLVM 8 REQUIRED)

set(CMAKE_CXX_STANDARD 17)

add_library(LLVMTargetClones MODULE targetclones.cpp)
target_compile_options(LLVMTargetClones PRIVATE -fno-rtti)
target_compile_definitions(LLVMTargetClones PRIVATE ${LLVM_DEFINITIONS})
target_include_directories(LLVMTargetClones PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_options(LLVMTargetClones PRIVATE -Wl,-znodelete)
