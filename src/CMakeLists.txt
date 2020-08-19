find_package(LLVM 8 REQUIRED)

set(CMAKE_CXX_STANDARD 17)

add_subdirectory(cpumodel)

add_library(LLVMTargetClones MODULE targetclones.cpp)
add_dependencies(LLVMTargetClones cpumodel_h)
target_compile_definitions(LLVMTargetClones PRIVATE ${LLVM_DEFINITIONS})
target_include_directories(LLVMTargetClones PRIVATE ${LLVM_INCLUDE_DIRS}
                                                    ${CMAKE_CURRENT_BINARY_DIR}/cpumodel)

# https://github.com/izenecloud/cmake/blob/master/SetCompilerWarningAll.cmake
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # Use the highest warning level for Visual Studio.
    set(CMAKE_CXX_WARNING_LEVEL 4)
    if(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
        string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    else(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
    endif(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
    # Disable C++ exceptions.
    string(REGEX REPLACE "/EH[a-z]+" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
    # Disable RTTI.
    string(REGEX REPLACE "/GR" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GR-")
else(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # Use -Wall for clang and gcc.
    if(NOT CMAKE_CXX_FLAGS MATCHES "-Wall")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
    endif(NOT CMAKE_CXX_FLAGS MATCHES "-Wall")
    # Use -Werror for clang and gcc.
    if(NOT CMAKE_CXX_FLAGS MATCHES "-Werror")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
    endif(NOT CMAKE_CXX_FLAGS MATCHES "-Werror")
    # Disable C++ exceptions.
    string(REGEX REPLACE "-fexceptions" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")
    # Disable RTTI.
    string(REGEX REPLACE "-frtti" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
    if(CMAKE_SYSTEM_NAME STREQUAL Linux)
        target_link_options(LLVMTargetClones PRIVATE -Wl,-znodelete)
    endif()
endif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
