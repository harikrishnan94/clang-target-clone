#!/usr/bin/env bash

# CLANG8 must point to valid clang8 compiler

MY_PATH="`dirname \"$0\"`"
cd "$MY_PATH"

$CLANG8 test.c -O3 -S -emit-llvm \
    -Xclang -load -Xclang ../build/src/libLLVMTargetClones.so
