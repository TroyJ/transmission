add_library(vdkqueue STATIC)
set_target_properties(vdkqueue PROPERTIES CXX_CLANG_TIDY "" OBJCXX_CLANG_TIDY "")

target_sources(vdkqueue
    PRIVATE
        VDKQueue.h
        VDKQueue.mm)

set_property(
    TARGET vdkqueue
    PROPERTY FOLDER "${TR_THIRD_PARTY_DIR_NAME}")

target_include_directories(vdkqueue
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})
