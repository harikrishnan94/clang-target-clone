#!/usr/bin/env bash

# CLANG8 must point to valid clang8 compiler

MY_PATH="`dirname \"$0\"`"
cd "$MY_PATH"

$CLANG8 test.c -S -g -emit-llvm -O3 -fplugin=../build/src/libLLVMTargetClones.so
$CLANG8 test.c -S -emit-llvm -O3 -fplugin=../build/src/libLLVMTargetClones.so