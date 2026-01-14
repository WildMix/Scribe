cmake_minimum_required(VERSION 3.16)

project(scribe
    VERSION 0.1.0
    DESCRIPTION "A protocol for Verifiable Data Lineage"
    LANGUAGES C
)

# C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build options
option(SCRIBE_BUILD_TESTS "Build test suite" ON)
option(SCRIBE_WITH_POSTGRES "Build with PostgreSQL support" ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Compiler warnings
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Find required packages
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)

if(SCRIBE_WITH_POSTGRES)
    find_package(PostgreSQL REQUIRED)
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/vendor)

# Vendor library: yyjson
add_library(yyjson STATIC
    vendor/yyjson/yyjson.c
)
target_include_directories(yyjson PUBLIC vendor/yyjson)

# Core library
add_library(scribe_core STATIC
    src/core/hash.c
    src/core/error.c
    src/util/path.c
    src/util/log.c
    src/util/json.c
    src/core/object.c
    src/core/merkle.c
    src/core/envelope.c
    src/storage/database.c
    src/storage/objects.c
    src/storage/repository.c
)

target_link_libraries(scribe_core
    PUBLIC
        OpenSSL::Crypto
        SQLite::SQLite3
        yyjson
)

# PostgreSQL support library
if(SCRIBE_WITH_POSTGRES)
    add_library(scribe_postgres STATIC
        src/postgres/pg_client.c
        src/postgres/pg_monitor.c
        src/postgres/pg_trigger.c
        src/postgres/pg_logical.c
    )

    target_link_libraries(scribe_postgres
        PUBLIC
            scribe_core
            PostgreSQL::PostgreSQL
    )

    target_compile_definitions(scribe_postgres PUBLIC SCRIBE_WITH_POSTGRES)
endif()

# Command implementations
add_library(scribe_commands STATIC
    src/commands/cmd_init.c
    src/commands/cmd_commit.c
    src/commands/cmd_log.c
    src/commands/cmd_status.c
    src/commands/cmd_verify.c
)

target_link_libraries(scribe_commands PUBLIC scribe_core)

if(SCRIBE_WITH_POSTGRES)
    target_sources(scribe_commands PRIVATE src/commands/cmd_watch.c)
    target_link_libraries(scribe_commands PUBLIC scribe_postgres)
endif()

# Main executable
add_executable(scribe src/main.c)

target_link_libraries(scribe
    PRIVATE
        scribe_commands
        scribe_core
)

if(SCRIBE_WITH_POSTGRES)
    target_link_libraries(scribe PRIVATE scribe_postgres)
endif()

# Installation
install(TARGETS scribe RUNTIME DESTINATION bin)
install(DIRECTORY include/scribe DESTINATION include)

# Print configuration summary
message(STATUS "")
message(STATUS "Scribe ${PROJECT_VERSION} configuration:")
message(STATUS "  Build type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  PostgreSQL:       ${SCRIBE_WITH_POSTGRES}")
message(STATUS "  Build tests:      ${SCRIBE_BUILD_TESTS}")
message(STATUS "")
