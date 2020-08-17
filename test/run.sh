#!/usr/bin/env bash

# CLANG8 must point to valid clang8 compiler

MY_PATH="`dirname \"$0\"`"
cd "$MY_PATH"

$CLANG8 test.c -S -emit-llvm -fplugin=../build/src/libLLVMTargetClones.so
