cmake_minimum_required(VERSION 3.14)
project(jk_proto_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_executable(test_jk_proto
    src/jk/jk_proto.c
    tests/test_jk_proto.c
)

target_include_directories(test_jk_proto PRIVATE src/jk src)

add_test(NAME jk_proto COMMAND test_jk_proto)

add_executable(test_classic_codec
    src/classic/classic_codec.c
    tests/test_classic_codec.c
)

target_include_directories(test_classic_codec PRIVATE src/classic src)

add_test(NAME classic_codec COMMAND test_classic_codec)
