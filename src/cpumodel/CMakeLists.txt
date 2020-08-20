find_program(
    CLANG
    NAMES clang-8 clang clang-9 clang-10
    PATHS ENV LLVM_PATH)
if(CLANG)
    message(STATUS "clang found : ${CLANG}")
else()
    message(FATAL_ERROR "Can't found program: clang")
endif()

if(WIN32)
    set(BC_CXXFLAGS -std=c++17 -O3 -fno-rtti -c -emit-llvm)
else()
    set(BC_CXXFLAGS -std=c++17 -O3 -fno-rtti -c -emit-llvm -fPIC)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(BC_CXXFLAGS ${BC_CXXFLAGS} -DNDEBUG)
endif()

add_custom_target(
    cpumodel_bc ALL
    COMMAND ${CLANG} ${BC_CXXFLAGS} -o ${CMAKE_CURRENT_BINARY_DIR}/cpumodel.bc
            ${CMAKE_CURRENT_SOURCE_DIR}/cpumodel.cpp
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cpumodel.cpp
    VERBATIM)

add_subdirectory(embed)

add_custom_target(
    cpumodel_h ALL
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/embed/embed ${CMAKE_CURRENT_BINARY_DIR}/cpumodel.bc
            ${CMAKE_CURRENT_BINARY_DIR}/cpumodel.bc.h
    DEPENDS cpumodel_bc embed)
