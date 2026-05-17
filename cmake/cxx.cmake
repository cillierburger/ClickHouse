# Enable libcxx hardening, see https://libcxx.llvm.org/Hardening.html
# In Debug we use EXTENSIVE (broadest checks). In Release we use NONE — FAST mode adds runtime
# bounds/internal-invariant checks throughout libc++ (vector::operator[], shared_ptr, etc.) that
# show up as constant overhead on hot paths (CREATE TABLE, AST parsing, etc.). Production safety
# is provided by the build's -fstrict-vtable-pointers and ClickHouse's own assertions; the libc++
# layer of checks is not worth the per-operation cost in benchmarks dominated by STL access.
if (CMAKE_BUILD_TYPE_UC STREQUAL "DEBUG")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE")
else ()
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_NONE")
endif ()

disable_dummy_launchers_if_needed()
add_subdirectory(contrib/libcxxabi-cmake)
add_subdirectory(contrib/libcxx-cmake)
enable_dummy_launchers_if_needed()

# Exception handling library is embedded into libcxxabi.

target_link_libraries(global-libs INTERFACE cxx cxxabi)
