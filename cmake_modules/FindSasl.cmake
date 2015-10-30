# Copyright 2015 Cloudera, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# - Find Cyrus SASL (sasl.h, libsasl2.so)
#
# This module defines
#  CYRUS_SASL_INCLUDE_DIR, directory containing headers
#  CYRUS_SASL_SHARED_LIB, path to libsasl2.so
#  CYRUS_SASL_LIB_DEPS other libraries that SASL depends on
#  CYRUS_SASL_FOUND, whether Cyrus SASL and its plugins have been found
#
# N.B: we do _not_ include sasl in thirdparty, for a fairly subtle reason. The
# TLDR version is that newer versions of cyrus-sasl (>=2.1.26) have a bug fix
# for https://bugzilla.cyrusimap.org/show_bug.cgi?id=3590, but that bug fix
# relied on a change both on the plugin side and on the library side. If you
# then try to run the new version of sasl (e.g from our thirdparty tree) with
# an older version of a plugin (eg from RHEL6 install), you'll get a SASL_NOMECH
# error due to this bug.
#
# In practice, Cyrus-SASL is so commonly used and generally non-ABI-breaking that
# we should be OK to depend on the host installation.


find_path(SASL_INCLUDE_DIR sasl/sasl.h)
find_library(SASL_SHARED_LIB NAMES "libsasl2${CMAKE_SHARED_LIBRARY_SUFFIX}")

if (SASL_INCLUDE_DIR AND SASL_SHARED_LIB)
  set(CYRUS_SASL_FOUND TRUE)
else ()
  set(CYRUS_SASL_FOUND FALSE)
endif ()

if (CYRUS_SASL_FOUND)
  if (NOT APPLE)
    set(CYRUS_SASL_LIB_DEPS dl crypt)
  else ()
    # the crypt function is in the system C library: no special linker options required.
    set(CYRUS_SASL_LIB_DEPS dl)
  endif ()
  if (NOT CyrusSASL_FIND_QUIETLY)
    message(STATUS "Found the CyrusSASL library: ${CYRUS_SASL_SHARED_LIB}")
  endif ()
else ()
  if (NOT Sasl_FIND_QUIETLY)
    set(CYRUS_SASL_ERR_MSG "Could not find the CyrusSASL Library.")
    set(CYRUS_SASL_ERR_MSG "Install libsasl2-dev or cyrus-sasl-devel packages to build.")
    if (Sasl_FIND_REQUIRED)
      message(FATAL_ERROR "${CYRUS_SASL_ERR_MSG}")
    else (Sasl_FIND_REQUIRED)
      message(STATUS "${CYRUS_SASL_ERR_MSG}")
    endif (Sasl_FIND_REQUIRED)
  endif ()
endif ()

mark_as_advanced(
  SASL_INCLUDE_DIR
  SASL_SHARED_LIB
  SASL_LIB_DEPS
)
