#!/bin/bash
set -e

export LANG=zh_CN.UTF-8
export LC_ALL=zh_CN.UTF-8

ROOT_DIR="$(pwd)"
BUILD_DIR="cmake-build-local"
LOCAL_HOME="${ROOT_DIR}/.home"
LOCAL_TMP="${ROOT_DIR}/.tmp"

mkdir -p "${LOCAL_HOME}" "${LOCAL_TMP}"

export HOME="${LOCAL_HOME}"
export TMPDIR="${LOCAL_TMP}"
export TMP="${LOCAL_TMP}"
export TEMP="${LOCAL_TMP}"

rm -rf "${BUILD_DIR}"
mkdir "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake -G "Ninja" \
  -DCMAKE_C_COMPILER="E:/WorkComp/msys64/ucrt64/bin/gcc.exe" \
  -DCMAKE_CXX_COMPILER="E:/WorkComp/msys64/ucrt64/bin/g++.exe" \
  -DNPM="E:/work/c++/Sunshine/npm_wrapper.cmd" \
  -DBUILD_DOCS=OFF \
  -DBUILD_TESTS=OFF \
  ..

ninja