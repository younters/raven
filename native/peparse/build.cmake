# The PE reader (native/peparse), in Rust: cargo builds the static library raven_pe reads files through.
find_program(RAVEN_CARGO cargo REQUIRED)
file(GLOB RAVEN_PEPARSE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/src/*.rs)
set(RAVEN_PEPARSE_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/peparse/release/libraven_peparse.a)
add_custom_command(OUTPUT ${RAVEN_PEPARSE_LIBRARY}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/peparse
        ${RAVEN_CARGO} build --locked --release --lib --manifest-path ${CMAKE_CURRENT_LIST_DIR}/Cargo.toml
    DEPENDS ${RAVEN_PEPARSE_SOURCES} ${CMAKE_CURRENT_LIST_DIR}/Cargo.toml ${CMAKE_CURRENT_LIST_DIR}/Cargo.lock
    VERBATIM)
add_custom_target(raven_peparse_library DEPENDS ${RAVEN_PEPARSE_LIBRARY})
add_library(raven_peparse STATIC IMPORTED)
set_target_properties(raven_peparse PROPERTIES IMPORTED_LOCATION ${RAVEN_PEPARSE_LIBRARY} INTERFACE_LINK_LIBRARIES "pthread;dl;m")
add_dependencies(raven_peparse raven_peparse_library)
