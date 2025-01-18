cmake_minimum_required(VERSION 3.15)
project(ReVPK)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find required packages
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(zstd REQUIRED)

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
        ${CMAKE_SOURCE_DIR}/lzham_alpha/include  # LZHAM public headers
        ${ZSTD_INCLUDE_DIRS}
)

# Link against libraries
target_link_libraries(revpk
    PUBLIC
        ${ZLIB_LIBRARIES}
        OpenSSL::SSL
        OpenSSL::Crypto
        zstd::libzstd
    PRIVATE
        lzhamdll
        lzhamcomp
        lzhamdecomp
)
