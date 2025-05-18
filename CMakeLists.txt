cmake_minimum_required(VERSION 3.15)
project(ReVPK)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2 -g")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -O2 -g")

# Find required packages
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED libxxhash)
pkg_check_modules(ZSTD REQUIRED libzstd)

# Add LZHAM subdirectory
add_subdirectory(lzham_alpha)

# Include directories
add_executable(revpk
    revpk.cpp
    packedstore.cpp
    keyvalues.cpp
)

target_include_directories(revpk
    PUBLIC
        ${ZLIB_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/lzham_alpha/include
        ${ZSTD_INCLUDE_DIRS}
        ${XXHASH_INCLUDE_DIRS}  # Add this line
)

# Link against libraries
target_link_libraries(revpk
    PUBLIC
        ${ZLIB_LIBRARIES}
        OpenSSL::SSL
        OpenSSL::Crypto
        ${ZSTD_LIBRARIES}
        ${XXHASH_LIBRARIES}  # Add this line
    PRIVATE
        lzhamdll
        lzhamcomp
        lzhamdecomp
)
