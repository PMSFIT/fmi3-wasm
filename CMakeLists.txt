cmake_minimum_required(VERSION 3.20)
project(fmi3-wasm NONE)

# ---------------------------------------------------------------------------
# Find Cargo (Rust build tool) – required for building FMU examples
# ---------------------------------------------------------------------------
find_program(CARGO_EXECUTABLE cargo
    HINTS ENV CARGO_HOME
    PATH_SUFFIXES bin
    DOC "Path to the cargo build tool"
)

if(NOT CARGO_EXECUTABLE)
    message(WARNING
        "cargo not found – FMU example targets will be skipped.\n"
        "Install Rust from https://rustup.rs/ to enable them."
    )
endif()

# ---------------------------------------------------------------------------
# Find wasm-tools
# ---------------------------------------------------------------------------
find_program(WASM_TOOLS_EXECUTABLE wasm-tools
    HINTS ENV WASM_TOOLS_PATH
    DOC "Path to the wasm-tools binary"
)

if(NOT WASM_TOOLS_EXECUTABLE)
    message(FATAL_ERROR
        "wasm-tools not found. Install it with:\n"
        "  cargo install wasm-tools\n"
        "or set WASM_TOOLS_PATH to the directory containing wasm-tools."
    )
endif()

message(STATUS "Found wasm-tools: ${WASM_TOOLS_EXECUTABLE}")

# ---------------------------------------------------------------------------
# Collect all .wit files under wit/
# ---------------------------------------------------------------------------
file(GLOB_RECURSE WIT_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/wit/*.wit")

if(NOT WIT_FILES)
    message(FATAL_ERROR "No .wit files found under ${CMAKE_SOURCE_DIR}/wit/")
endif()

message(STATUS "WIT files to validate: ${WIT_FILES}")

# ---------------------------------------------------------------------------
# Custom target: validate-wit
#
# wasm-tools component wit <dir> parses and resolves the entire WIT package
# in the given directory, reporting any syntax or resolution errors.
# ---------------------------------------------------------------------------
add_custom_target(validate-wit ALL
    COMMAND ${WASM_TOOLS_EXECUTABLE} component wit "${CMAKE_SOURCE_DIR}/wit"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Validating WIT files with wasm-tools..."
    VERBATIM
)

# ---------------------------------------------------------------------------
# Also expose a standalone 'wit-check' target that is not part of ALL,
# so developers can run it explicitly:  cmake --build . --target wit-check
# ---------------------------------------------------------------------------
add_custom_target(wit-check
    COMMAND ${WASM_TOOLS_EXECUTABLE} component wit "${CMAKE_SOURCE_DIR}/wit"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Checking WIT files with wasm-tools..."
    VERBATIM
)

# ---------------------------------------------------------------------------
# CTest integration – running ctest will also validate the WIT files
# ---------------------------------------------------------------------------
enable_testing()

add_test(
    NAME wit-validation
    COMMAND ${WASM_TOOLS_EXECUTABLE} component wit "${CMAKE_SOURCE_DIR}/wit"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# ---------------------------------------------------------------------------
# Example FMU: adder-fmu
#
# A Co-Simulation FMU with two Float64 inputs and one Float64 output
# (sum = input_a + input_b), compiled to a Wasm component.
#
# Build:   cmake --build <build-dir> --target adder-fmu
# Output:  examples/adder-fmu/target/wasm32-wasip2/release/adder_fmu.wasm
# ---------------------------------------------------------------------------
if(CARGO_EXECUTABLE)
    set(ADDER_FMU_DIR  "${CMAKE_SOURCE_DIR}/examples/adder-fmu")
    set(ADDER_FMU_WASM
        "${ADDER_FMU_DIR}/target/wasm32-wasip2/release/adder_fmu.wasm"
    )

    add_custom_command(
        OUTPUT  "${ADDER_FMU_WASM}"
        COMMAND ${CARGO_EXECUTABLE} build --release
        WORKING_DIRECTORY "${ADDER_FMU_DIR}"
        COMMENT "Building adder-fmu Wasm component..."
        VERBATIM
        # Rebuild when any source or WIT file changes.
        DEPENDS
            "${ADDER_FMU_DIR}/Cargo.toml"
            "${ADDER_FMU_DIR}/src/lib.rs"
            ${WIT_FILES}
    )

    add_custom_target(adder-fmu
        DEPENDS "${ADDER_FMU_WASM}"
    )

    # Validate the compiled component against the WIT package.
    add_custom_target(validate-adder-fmu
        COMMAND ${WASM_TOOLS_EXECUTABLE} validate --features component-model
                "${ADDER_FMU_WASM}"
        DEPENDS adder-fmu
        COMMENT "Validating adder-fmu Wasm component..."
        VERBATIM
    )

    add_test(
        NAME adder-fmu-wasm-validation
        COMMAND ${WASM_TOOLS_EXECUTABLE} validate --features component-model
                "${ADDER_FMU_WASM}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(adder-fmu-wasm-validation
        PROPERTIES DEPENDS wit-validation
    )
endif()
