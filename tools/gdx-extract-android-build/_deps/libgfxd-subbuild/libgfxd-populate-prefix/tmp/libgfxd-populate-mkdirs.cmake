# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/Users/Quentin/Documents/vrmods/fzero/tools/gdx-extract-android-build/_deps/libgfxd-src")
  file(MAKE_DIRECTORY "C:/Users/Quentin/Documents/vrmods/fzero/tools/gdx-extract-android-build/_deps/libgfxd-src")
endif()
file(MAKE_DIRECTORY
  "C:/Users/Quentin/Documents/vrmods/fzero/tools/gdx-extract-android-build/_deps/libgfxd-build"
  "C:/Users/Quentin/Documents/vrmods/fzero/tools/gdx-extract-android-build/_deps/libgfxd-subbuild/libgfxd-populate-prefix"
  "C:/Users/Quentin/Documents/vrmods/fzero/tools/gdx-extract-android-build/_deps/libgfxd-subbuild/libgfxd-populate-prefix/tmp"
  "C:/Users/Quentin/Documents/vrmods/fzero/tools/gdx-extract-android-build/_deps/libgfxd-subbuild/libgfxd-populate-prefix/src/libgfxd-populate-stamp"
  "C:/Users/Quentin/Documents/vrmods/fzero/tools/gdx-extract-android-build/_deps/libgfxd-subbuild/libgfxd-populate-prefix/src"
  "C:/Users/Quentin/Documents/vrmods/fzero/tools/gdx-extract-android-build/_deps/libgfxd-subbuild/libgfxd-populate-prefix/src/libgfxd-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/Quentin/Documents/vrmods/fzero/tools/gdx-extract-android-build/_deps/libgfxd-subbuild/libgfxd-populate-prefix/src/libgfxd-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/Quentin/Documents/vrmods/fzero/tools/gdx-extract-android-build/_deps/libgfxd-subbuild/libgfxd-populate-prefix/src/libgfxd-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
