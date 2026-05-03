cmake_minimum_required(VERSION 3.20)

project(scribe VERSION 1.1.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_definitions(_GNU_SOURCE _POSIX_C_SOURCE=200809L)

option(SCRIBE_BUILD_MONGO_ADAPTER "Build MongoDB adapter" ON)
option(SCRIBE_BUILD_TESTS "Build tests" ON)
option(SCRIBE_ENABLE_ASAN "Enable ASan/UBSan" OFF)
option(SCRIBE_ENABLE_TSAN "Enable TSan" OFF)
option(SCRIBE_ENABLE_TESTCONTAINERS "Build Docker-backed Testcontainers integration tests" OFF)
option(SCRIBE_VENDORED_ZSTD "Use vendored zstd" ON)

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
    src/core/commit.c
    src/core/config.c
    src/core/context.c
    src/core/diff.c
    src/core/fs.c
    src/core/fsck.c
    src/core/inspect.c
    src/core/object.c
    src/core/pipe.c
    src/core/ref.c
    src/core/tree.c)

set(SCRIBE_MONGO_SOURCES
    src/adapter_mongo/mongo_bootstrap.c
    src/adapter_mongo/mongo_json.c)

add_library(scribe_core STATIC ${SCRIBE_UTIL_SOURCES} ${SCRIBE_CORE_SOURCES})
target_include_directories(scribe_core
    PUBLIC include
    PRIVATE src vendor/zstd/lib vendor/blake3/c)
target_link_libraries(scribe_core PUBLIC blake3 libzstd_static PRIVATE scribe_warnings)
scribe_apply_sanitizers(scribe_core)

add_library(scribe SHARED src/core/library.c)
target_include_directories(scribe PUBLIC include PRIVATE src)
target_link_libraries(scribe PRIVATE scribe_core scribe_warnings)
set_target_properties(scribe PROPERTIES OUTPUT_NAME scribe)
scribe_apply_sanitizers(scribe)

add_executable(scribe_cli src/cli/main.c)
target_include_directories(scribe_cli PRIVATE src)
target_link_libraries(scribe_cli PRIVATE scribe_core scribe_warnings)
set_target_properties(scribe_cli PROPERTIES OUTPUT_NAME scribe)
scribe_apply_sanitizers(scribe_cli)

if(SCRIBE_BUILD_MONGO_ADAPTER)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MONGOC QUIET libmongoc-1.0)
        pkg_check_modules(BSON QUIET libbson-1.0)
    endif()
    if(NOT MONGOC_FOUND OR NOT BSON_FOUND)
        message(WARNING "libmongoc/libbson not found; mongo-watch is disabled for this build")
        set(SCRIBE_BUILD_MONGO_ADAPTER OFF CACHE BOOL "" FORCE)
    else()
        add_library(scribe_mongo_adapter STATIC ${SCRIBE_MONGO_SOURCES})
        target_include_directories(scribe_mongo_adapter
            PUBLIC include
            PRIVATE src vendor/blake3/c)
        target_include_directories(scribe_mongo_adapter SYSTEM PRIVATE ${MONGOC_INCLUDE_DIRS} ${BSON_INCLUDE_DIRS})
        target_compile_options(scribe_mongo_adapter PRIVATE ${MONGOC_CFLAGS_OTHER} ${BSON_CFLAGS_OTHER})
        target_link_directories(scribe_mongo_adapter PRIVATE ${MONGOC_LIBRARY_DIRS} ${BSON_LIBRARY_DIRS})
        target_link_libraries(scribe_mongo_adapter
            PRIVATE scribe_core blake3 ${MONGOC_LIBRARIES} ${BSON_LIBRARIES} scribe_warnings)
        scribe_apply_sanitizers(scribe_mongo_adapter)
        target_compile_definitions(scribe_cli PRIVATE SCRIBE_HAVE_MONGO_ADAPTER=1)
        target_include_directories(scribe_cli SYSTEM PRIVATE ${MONGOC_INCLUDE_DIRS} ${BSON_INCLUDE_DIRS})
        target_link_directories(scribe_cli PRIVATE ${MONGOC_LIBRARY_DIRS} ${BSON_LIBRARY_DIRS})
        target_link_libraries(scribe_cli PRIVATE scribe_mongo_adapter ${MONGOC_LIBRARIES} ${BSON_LIBRARIES})
    endif()
endif()

if(SCRIBE_BUILD_TESTS)
    enable_testing()
    add_library(unity STATIC vendor/unity/src/unity.c)
    target_include_directories(unity PUBLIC vendor/unity/src)

    add_executable(scribe_tests
        tests/unit/test_main.c
        tests/unit/test_core.c)
    if(SCRIBE_BUILD_MONGO_ADAPTER)
        target_sources(scribe_tests PRIVATE tests/unit/test_mongo.c)
        target_compile_definitions(scribe_tests PRIVATE SCRIBE_HAVE_MONGO_ADAPTER=1)
        target_include_directories(scribe_tests SYSTEM PRIVATE ${MONGOC_INCLUDE_DIRS} ${BSON_INCLUDE_DIRS})
        target_link_directories(scribe_tests PRIVATE ${MONGOC_LIBRARY_DIRS} ${BSON_LIBRARY_DIRS})
        target_link_libraries(scribe_tests PRIVATE scribe_mongo_adapter ${MONGOC_LIBRARIES} ${BSON_LIBRARIES})
    endif()
    target_include_directories(scribe_tests PRIVATE src)
    target_link_libraries(scribe_tests PRIVATE scribe_core unity scribe_warnings)
    scribe_apply_sanitizers(scribe_tests)
    add_test(NAME scribe_tests COMMAND scribe_tests)
    add_test(NAME scribe_cli_features
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_cli_features.sh" $<TARGET_FILE:scribe_cli>)

    if(SCRIBE_ENABLE_TESTCONTAINERS)
        if(NOT SCRIBE_BUILD_MONGO_ADAPTER)
            message(FATAL_ERROR "SCRIBE_ENABLE_TESTCONTAINERS requires SCRIBE_BUILD_MONGO_ADAPTER=ON")
        endif()
        if(CMAKE_VERSION VERSION_LESS 3.26)
            message(FATAL_ERROR "Testcontainers Native requires CMake 3.26 or newer")
        endif()

        find_program(DOCKER_EXECUTABLE docker REQUIRED)

        include(FetchContent)
        set(SKIP_DEMOS TRUE CACHE BOOL "" FORCE)
        FetchContent_Declare(testcontainers_native
            GIT_REPOSITORY https://github.com/testcontainers/testcontainers-native.git
            GIT_TAG 2f659b90fedb74a3227a5a4171ef2b6f61195e4c)
        FetchContent_MakeAvailable(testcontainers_native)

        add_executable(scribe_mongo_integration_tests
            tests/integration/test_mongo_testcontainers.c)
        target_include_directories(scribe_mongo_integration_tests
            PRIVATE src)
        target_include_directories(scribe_mongo_integration_tests
            SYSTEM PRIVATE ${MONGOC_INCLUDE_DIRS} ${BSON_INCLUDE_DIRS})
        target_link_directories(scribe_mongo_integration_tests
            PRIVATE ${MONGOC_LIBRARY_DIRS} ${BSON_LIBRARY_DIRS})
        target_link_libraries(scribe_mongo_integration_tests
            PRIVATE testcontainers-c ${MONGOC_LIBRARIES} ${BSON_LIBRARIES} scribe_warnings)
        scribe_apply_sanitizers(scribe_mongo_integration_tests)

        add_test(NAME scribe_testcontainers_mongo_image
            COMMAND ${DOCKER_EXECUTABLE} build -t scribe-mongo-rs-test:7
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/mongo-rs")
        set_tests_properties(scribe_testcontainers_mongo_image PROPERTIES
            FIXTURES_SETUP scribe_mongo_image
            LABELS "integration;mongo;testcontainers")

        add_test(NAME scribe_mongo_testcontainers
            COMMAND scribe_mongo_integration_tests $<TARGET_FILE:scribe_cli>)
        set_tests_properties(scribe_mongo_testcontainers PROPERTIES
            FIXTURES_REQUIRED scribe_mongo_image
            LABELS "integration;mongo;testcontainers"
            TIMEOUT 180)
    endif()
endif()
