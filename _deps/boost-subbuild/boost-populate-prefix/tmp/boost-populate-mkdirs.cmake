# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "E:/work/c++/Sunshine/_deps/boost-src")
  file(MAKE_DIRECTORY "E:/work/c++/Sunshine/_deps/boost-src")
endif()
file(MAKE_DIRECTORY
  "E:/work/c++/Sunshine/_deps/boost-build"
  "E:/work/c++/Sunshine/_deps/boost-subbuild/boost-populate-prefix"
  "E:/work/c++/Sunshine/_deps/boost-subbuild/boost-populate-prefix/tmp"
  "E:/work/c++/Sunshine/_deps/boost-subbuild/boost-populate-prefix/src/boost-populate-stamp"
  "E:/work/c++/Sunshine/_deps/boost-subbuild/boost-populate-prefix/src"
  "E:/work/c++/Sunshine/_deps/boost-subbuild/boost-populate-prefix/src/boost-populate-stamp"
)

set(configSubDirs Debug)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "E:/work/c++/Sunshine/_deps/boost-subbuild/boost-populate-prefix/src/boost-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "E:/work/c++/Sunshine/_deps/boost-subbuild/boost-populate-prefix/src/boost-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
