#	Copyright (c) 2021, Aleksandr Platonov
#
#	This source code is released for free distribution under the terms
#	of the GNU General Public License version 2 or (at your option) any
#	later version.

cmake_minimum_required(VERSION 3.16)

project(ctags)

include(CheckFunctionExists)
include(CheckIncludeFile)

option(ENABLE_STATIC "Enable static build" ON)
option(DISABLE_XML "Disable XML support" OFF)
option(DISABLE_JSON "Disable JSON support" OFF)
option(DISABLE_SECCOMP "Disable seccomp" OFF)
option(DISABLE_INTERNAL_SORT "Use external sort" ON)
option(DISABLE_ICONV "Disable iconv" OFF)
option(DISABLE_YAML "Disable yaml" OFF)

if (ENABLE_STATIC)
  if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  else()
    set(LIBS -static)
  endif()
  # FIXME: We could not link libxml2 statically
  set(DISABLE_XML ON)
else()
  list(REMOVE_ITEM CMAKE_C_FLAGS -static)
endif()

set(XML_SRCS parsers/maven2.c
             parsers/dbusintrospect.c
             parsers/glade.c
             parsers/svg.c
             parsers/plist.c
             parsers/relaxng.c
             parsers/xml.c
             parsers/xslt.c)
set(YAML_SRCS parsers/yaml.c
              parsers/ansibleplaybook.c)

file(GLOB MAIN_SRCS RELATIVE ${CMAKE_SOURCE_DIR} main/*.c)
list(REMOVE_ITEM MAIN_SRCS main/mini-geany.c)
file(GLOB OPTLIB_SRCS optlib/*.c)
file(GLOB PARSERS_CXX_SRCS parsers/cxx/*.c)
file(GLOB PARSERS_SRCS RELATIVE ${CMAKE_SOURCE_DIR} parsers/*.c)
if (NOT DISABLE_XML)
  find_package(LibXml2 QUIET)
  if (NOT LibXml2_FOUND)
    message("libxml2 not found")
  else()
    message("Found libxml2: ${LIBXML2_VERSION_STRING}")
    set(HAVE_LIBXML 1)
    list(APPEND LIBS ${LIBXML2_LIBRARIES})
    include_directories(${LIBXML2_INCLUDE_DIRS})
  endif()
endif()
if (NOT HAVE_LIBXML)
  list(REMOVE_ITEM PARSERS_SRCS ${XML_SRCS})
endif()
check_include_file(yaml.h HAVE_LIBYAML)
if (NOT HAVE_LIBYAML)
  list(REMOVE_ITEM PARSERS_SRCS ${YAML_SRCS})
else()
  list(APPEND LIBS yaml)
endif()
check_include_file(jansson.h HAVE_JANSSON)
if (HAVE_JANSSON)
  list(APPEND LIBS jansson)
endif()
if (DISABLE_INTERNAL_SORT)
  set(EXTERNAL_SORT 1)
  check_function_exists(setenv HAVE_SETENV)
  check_function_exists(putenv HAVE_PUTENV)
endif()
check_include_file(direct.h HAVE_DIRECT_H)
check_include_file(dirent.h HAVE_DIRENT_H)
check_include_file(fcntl.h HAVE_FCNTL_H)
check_include_file(io.h HAVE_IO_H)
check_include_file(stat.h HAVE_STAT_H)
check_include_file(types.h HAVE_TYPES_H)
check_include_file(unistd.h HAVE_UNISTD_H)
check_include_file(sys/dir.h HAVE_SYS_DIR_H)
check_include_file(sys/stat.h HAVE_SYS_STAT_H)
check_include_file(sys/types.h HAVE_SYS_TYPES_H)
check_include_file(sys/wait.h HAVE_SYS_WAIT_H)
check_include_file(stdbool.h HAVE_STDBOOL_H)
check_include_file(regex.h HAVE_REGCOMP)
if (NOT HAVE_REGCOMP)
  set(REGEX_SRCS gnu_regex/regex.c)
  include_directories(gnu_regex)
  add_definitions(-DHAVE_REGCOMP -D__USE_GNU)
else()
  if (NOT MSVC)
    check_function_exists(regcomp HAVE_REGCOMP_LIBC)
    if (NOT HAVE_REGCOMP_LIBC)
      list(APPEND LIBS regex tre intl)
    endif()
  endif()
endif()
if (NOT DISABLE_ICONV)
  find_package(Iconv QUIET)
  if (Iconv_FOUND)
    set(HAVE_ICONV 1)
    list(APPEND LIBS ${Iconv_LIBRARIES})
    include_directories(${Iconv_INCLUDE_DIRS})
  endif()
endif()
check_include_file(fnmatch.h HAVE_FNMATCH_H)
if (NOT HAVE_FNMATCH_H)
  set(FNMATCH_SRCS fnmatch/fnmatch.c)
  include_directories(fnmatch)
endif()
check_include_file(seccomp.h HAVE_SECCOMP)
if (HAVE_SECCOMP)
  list(APPEND LIBS seccomp)
endif()

file(READ main/ctags.h CTAGS_H_CONTENTS)
string(REPLACE "\n" ";" CTAGS_H_LINES "${CTAGS_H_CONTENTS}")
foreach (CTAGS_H_LINE ${CTAGS_H_LINES})
  string(REGEX MATCH "PROGRAM_VERSION (\".*[0-9]\")" _ ${CTAGS_H_LINE})
  if (CMAKE_MATCH_1)
    set(PACKAGE_VERSION ${CMAKE_MATCH_1})
    break()
  endif()
endforeach()
check_function_exists(mkstemp HAVE_MKSTEMP)
if (NOT HAVE_MKSTEMP)
  set(MKSTEMP_SRCS win32/mkstemp/mkstemp.c)
endif()
if (WIN32)
  add_definitions(-DWIN32)
  if (MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS
                    -Dstrcasecmp=stricmp
                    -D_CONSOLE)
  endif()
endif()

configure_file(config.h.in.cmake "${CMAKE_BINARY_DIR}/config.h")
include_directories(${CMAKE_BINARY_DIR})
add_definitions(-DHAVE_CONFIG_H)
include_directories(main)

add_executable(ctags
               ${MAIN_SRCS}
               ${OPTLIB_SRCS}
               ${PARSERS_SRCS}
               ${PARSERS_CXX_SRCS}
               ${MKSTEMP_SRCS}
               ${REGEX_SRCS}
               ${FNMATCH_SRCS})
target_link_libraries(ctags ${LIBS})
