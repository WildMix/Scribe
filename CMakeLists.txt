cmake_minimum_required(VERSION 3.20)

project(scribe VERSION 1.1.2 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_definitions(_GNU_SOURCE _POSIX_C_SOURCE=200809L)

option(SCRIBE_BUILD_TESTS "Build tests" ON)
option(SCRIBE_ENABLE_ASAN "Enable ASan/UBSan" OFF)
option(SCRIBE_ENABLE_TSAN "Enable TSan" OFF)
option(SCRIBE_ENABLE_SECURE_TRANSPORT "Build TLS and HTTP/2 remote transport support" ON)

if(SCRIBE_ENABLE_ASAN AND SCRIBE_ENABLE_TSAN)
    message(FATAL_ERROR "SCRIBE_ENABLE_ASAN and SCRIBE_ENABLE_TSAN are mutually exclusive")
endif()

function(scribe_apply_sanitizers target)
    if(SCRIBE_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC -fsanitize=address,undefined)
    elseif(SCRIBE_ENABLE_TSAN)
        target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC -fsanitize=thread)
    endif()
endfunction()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/blake3/c/blake3.h")
    message(FATAL_ERROR "Missing vendor/blake3 submodule. Run: git submodule update --init --recursive")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/zstd/lib/zstd.h")
    message(FATAL_ERROR "Missing vendor/zstd submodule. Run: git submodule update --init --recursive")
endif()
if(SCRIBE_BUILD_TESTS AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/unity/src/unity.h")
    message(FATAL_ERROR "Missing vendor/unity submodule. Run: git submodule update --init --recursive")
endif()

add_library(scribe_warnings INTERFACE)
target_compile_options(scribe_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)

add_library(blake3 STATIC
    vendor/blake3/c/blake3.c
    vendor/blake3/c/blake3_dispatch.c
    vendor/blake3/c/blake3_portable.c)
target_include_directories(blake3 PUBLIC vendor/blake3/c)
target_compile_definitions(blake3 PUBLIC
    BLAKE3_NO_SSE2
    BLAKE3_NO_SSE41
    BLAKE3_NO_AVX2
    BLAKE3_NO_AVX512
    BLAKE3_USE_NEON=0)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/zstd/build/cmake vendor/zstd-build EXCLUDE_FROM_ALL)

set(SCRIBE_UTIL_SOURCES
    src/util/arena.c
    src/util/error.c
    src/util/hex.c
    src/util/leb128.c
    src/util/log.c
    src/util/queue.c)

set(SCRIBE_CORE_SOURCES
    src/core/blob.c
    src/core/bundle.c
    src/core/checkpoint.c
    src/core/commit.c
    src/core/config.c
    src/core/context.c
    src/core/daemon.c
    src/core/diff.c
    src/core/fs.c
    src/core/fsck.c
    src/core/gc.c
    src/core/hash_set.c
    src/core/ingest.c
    src/core/inspect.c
    src/core/ndjson.c
    src/core/object.c
    src/core/pack.c
    src/core/pipe.c
    src/core/ref.c
    src/core/repository.c
    src/core/sync.c
    src/core/tree.c)

add_library(scribe_core STATIC ${SCRIBE_UTIL_SOURCES} ${SCRIBE_CORE_SOURCES})
set_target_properties(scribe_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(scribe_core
    PUBLIC include
    PRIVATE src vendor/zstd/lib vendor/blake3/c)
target_link_libraries(scribe_core PUBLIC blake3 libzstd_static PRIVATE scribe_warnings)
scribe_apply_sanitizers(scribe_core)

if(SCRIBE_ENABLE_SECURE_TRANSPORT)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(scribe_core PRIVATE SCRIBE_HAVE_TLS=1)
    target_link_libraries(scribe_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

add_library(scribe SHARED src/core/library.c)
target_include_directories(scribe PUBLIC include PRIVATE src)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_link_libraries(scribe PRIVATE "-Wl,--whole-archive" scribe_core "-Wl,--no-whole-archive" scribe_warnings)
else()
    target_link_libraries(scribe PRIVATE scribe_core scribe_warnings)
endif()
set_target_properties(scribe PROPERTIES OUTPUT_NAME scribe)
scribe_apply_sanitizers(scribe)

add_executable(scribe_cli src/cli/main.c vendor/linenoise/linenoise.c)
target_include_directories(scribe_cli PRIVATE src vendor/linenoise)
target_link_libraries(scribe_cli PRIVATE scribe_core scribe_warnings)
set_target_properties(scribe_cli PROPERTIES OUTPUT_NAME scribe)
scribe_apply_sanitizers(scribe_cli)

add_executable(scribe_remote_cli
    src/cli/main.c
    vendor/linenoise/linenoise.c)
target_compile_definitions(scribe_remote_cli PRIVATE SCRIBE_CLIENT_ONLY=1)
target_include_directories(scribe_remote_cli PRIVATE src vendor/linenoise)
target_link_libraries(scribe_remote_cli PRIVATE scribe_core scribe_warnings)
set_target_properties(scribe_remote_cli PROPERTIES OUTPUT_NAME scribe-cli)
scribe_apply_sanitizers(scribe_remote_cli)

if(SCRIBE_BUILD_TESTS)
    enable_testing()
    add_library(unity STATIC vendor/unity/src/unity.c)
    target_include_directories(unity PUBLIC vendor/unity/src)

    add_executable(scribe_tests tests/unit/test_main.c tests/unit/test_core.c)
    target_include_directories(scribe_tests PRIVATE src)
    target_link_libraries(scribe_tests PRIVATE scribe_core unity scribe_warnings)
    scribe_apply_sanitizers(scribe_tests)
    add_test(NAME scribe_tests COMMAND scribe_tests)
    add_test(NAME scribe_cli_features
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_cli_features.sh"
                $<TARGET_FILE:scribe_cli> $<TARGET_FILE:scribe_remote_cli>)
    set_tests_properties(scribe_cli_features PROPERTIES TIMEOUT 180)
    add_test(NAME scribe_release_script
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_release.sh"
                     "${CMAKE_CURRENT_SOURCE_DIR}/scripts/push-versioned.sh")
endif()
