cmake_minimum_required(VERSION 3.31)

project(ForgeGuestRuntime LANGUAGES C CXX)

include(GNUInstallDirs)

if(NOT FORGE_GUEST_INCLUDE_DIR OR NOT IS_DIRECTORY "${FORGE_GUEST_INCLUDE_DIR}")
   message(FATAL_ERROR "FORGE_GUEST_INCLUDE_DIR must contain generated contract headers")
endif()
if(NOT FORGE_GUEST_LIBC_SOURCE_DIR OR NOT IS_DIRECTORY "${FORGE_GUEST_LIBC_SOURCE_DIR}/src/math")
   message(FATAL_ERROR "FORGE_GUEST_LIBC_SOURCE_DIR must contain the pinned CDT musl sources")
endif()

set(_forge_guest_cstdlib_sources)
foreach(
   _source IN ITEMS
      ctype/isalnum.c ctype/isalpha.c ctype/isblank.c ctype/iscntrl.c
      ctype/isdigit.c ctype/isgraph.c ctype/islower.c ctype/isprint.c
      ctype/ispunct.c ctype/isspace.c ctype/isupper.c ctype/isxdigit.c
      ctype/tolower.c ctype/toupper.c
      exit/atexit.c thread/__lock.c
      internal/intscan.c internal/floatscan.c internal/shgetc.c
      locale/c_locale.c locale/localeconv.c locale/langinfo.c locale/__lctrans.c
      locale/strcoll.c locale/strxfrm.c
      stdio/vasprintf.c stdio/__uflow.c stdio/__toread.c
      stdio/fwrite.c stdio/__towrite.c
      stdio/sscanf.c stdio/vsscanf.c stdio/vfscanf.c stdio/__string_read.c
      multibyte/mbrtowc.c multibyte/mbsinit.c multibyte/internal.c multibyte/wcrtomb.c multibyte/wctomb.c
      string/wcslen.c string/strnlen.c
      stdlib/abs.c stdlib/strtol.c stdlib/strtod.c
      time/strftime.c time/__tm_to_secs.c time/__year_to_secs.c time/__month_to_secs.c
)
   list(APPEND _forge_guest_cstdlib_sources "${FORGE_GUEST_LIBC_SOURCE_DIR}/src/${_source}")
endforeach()
foreach(_source IN ITEMS vfprintf.c vsnprintf.c snprintf.c vsprintf.c sprintf.c printf.c vprintf.c)
   list(APPEND _forge_guest_cstdlib_sources "${CMAKE_CURRENT_SOURCE_DIR}/donors/musl-628cf979/${_source}")
endforeach()
add_library(forge_guest_cstdlib OBJECT ${_forge_guest_cstdlib_sources} stdio_lock.cpp stdio_console.cpp)
set_source_files_properties(
   "${FORGE_GUEST_LIBC_SOURCE_DIR}/src/stdlib/strtod.c"
   PROPERTIES COMPILE_DEFINITIONS
      "strtof_l=__forge_musl_untyped_strtof_l;strtod_l=__forge_musl_untyped_strtod_l;strtold_l=__forge_musl_untyped_strtold_l;__strtof_l=__forge_musl_untyped___strtof_l;__strtod_l=__forge_musl_untyped___strtod_l;__strtold_l=__forge_musl_untyped___strtold_l"
)
target_include_directories(
   forge_guest_cstdlib
   PRIVATE
      "${FORGE_GUEST_LIBC_SOURCE_DIR}/include"
      "${FORGE_GUEST_LIBC_SOURCE_DIR}/src/internal"
      "${FORGE_GUEST_LIBC_SOURCE_DIR}/arch/eos"
      "${FORGE_GUEST_INCLUDE_DIR}"
)
# Isolate the pinned libc implementation's private FILE and scalar declarations.
# They must not replace the public SDK C headers for contract compilation.
target_compile_options(
   forge_guest_cstdlib PRIVATE -nostdinc -ffreestanding -fvisibility=hidden -fno-ident -mcpu=mvp
      "$<$<COMPILE_LANGUAGE:C>:-include>"
      "$<$<COMPILE_LANGUAGE:C>:${CMAKE_CURRENT_SOURCE_DIR}/details/c_locale.hxx>"
)
target_compile_features(forge_guest_cstdlib PRIVATE c_std_11 cxx_std_23)

add_library(
   forge_guest_runtime STATIC
   $<TARGET_OBJECTS:forge_guest_cstdlib>
   allocator.cpp
   allocator_new_delete.cpp
   errno.cpp
   locale.cpp
   memory.cpp
)

target_include_directories(
   forge_guest_runtime
   PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}"
      "${FORGE_GUEST_INCLUDE_DIR}"
)
target_compile_features(forge_guest_runtime PUBLIC cxx_std_23)
target_compile_definitions(forge_guest_runtime PRIVATE FORGE_CONTRACT_GUEST=1)
target_compile_options(
   forge_guest_runtime
   PRIVATE
      -fno-exceptions
      -fno-rtti
      -fno-threadsafe-statics
      -ffreestanding
      -fvisibility=hidden
      -fno-ident
      -mcpu=mvp
)

install(TARGETS forge_guest_runtime ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

file(GLOB _forge_guest_math_sources CONFIGURE_DEPENDS "${FORGE_GUEST_LIBC_SOURCE_DIR}/src/math/*.c")
add_library(forge_guest_math STATIC ${_forge_guest_math_sources})
set_target_properties(forge_guest_math PROPERTIES OUTPUT_NAME m)
target_compile_features(forge_guest_math PRIVATE c_std_11)
target_include_directories(
   forge_guest_math
   PRIVATE
      "${FORGE_GUEST_LIBC_SOURCE_DIR}/src/internal"
      "${FORGE_GUEST_LIBC_SOURCE_DIR}/arch/eos"
)
target_compile_options(
   forge_guest_math
   PRIVATE
      -idirafter
      "${FORGE_GUEST_LIBC_SOURCE_DIR}/include"
      -ffreestanding
      -fvisibility=hidden
      -fno-ident
      -mcpu=mvp
)

install(TARGETS forge_guest_math ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
