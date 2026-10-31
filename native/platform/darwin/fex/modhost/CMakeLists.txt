# SPDX-License-Identifier: MIT
# Also supports standalone configuration, without provisioning Raven's fixtures.
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	project(RavenFexModhost LANGUAGES CXX ASM)
	enable_testing()
endif()
option(RAVEN_FEX_MODHOST "Build standalone experimental FEX module-host tests" OFF)
if(NOT RAVEN_FEX_MODHOST)
	return()
endif()
if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
	return()
endif()
set(RAVEN_FEX_MODULE_ROOT "$ENV{RAVEN_FEX_MODULE_ROOT}" CACHE PATH "Provisioned FEX modules; empty root skips execution")
enable_language(ASM)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(fex_modhost_substrate native_host.cpp abi.S context.S)
add_executable(fex_modhost_abi abi_test.cpp abi.S abi_fixture.S)
foreach(target fex_modhost_substrate fex_modhost_abi)
	target_compile_features(${target} PRIVATE cxx_std_17)
	target_compile_options(${target} PRIVATE -arch arm64 -ffixed-x18 -ffp-contract=off -Wall -Wextra -Werror -mmacosx-version-min=11.0)
	# Self-test predicates must remain active in Release builds.
	target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-UNDEBUG>)
	target_link_options(${target} PRIVATE -arch arm64 -Wl,-platform_version,macos,11.0,11.0)
endforeach()
get_filename_component(RAVEN_MODHOST_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
add_test(NAME fex_modhost_driver_contract COMMAND ${Python3_EXECUTABLE}
	"${RAVEN_MODHOST_REPO}/scripts/test-fex-modhost-contract.py")
set_tests_properties(fex_modhost_driver_contract PROPERTIES TIMEOUT 30 LABELS "fex;contract")
add_test(NAME fex_modhost_substrate COMMAND ${Python3_EXECUTABLE}
	"${RAVEN_MODHOST_REPO}/scripts/test-fex-modhost.py" --substrate-only
	--host $<TARGET_FILE:fex_modhost_substrate> --abi $<TARGET_FILE:fex_modhost_abi>)
set_tests_properties(fex_modhost_substrate PROPERTIES TIMEOUT 60 LABELS "fex;substrate")

set(RAVEN_FEX_MODULE_ARGUMENTS)
if(RAVEN_FEX_MODULE_ROOT)
	list(APPEND RAVEN_FEX_MODULE_ARGUMENTS --modules "${RAVEN_FEX_MODULE_ROOT}")
endif()
add_test(NAME fex_modhost_execution COMMAND ${Python3_EXECUTABLE}
	"${RAVEN_MODHOST_REPO}/scripts/test-fex-modhost.py"
	${RAVEN_FEX_MODULE_ARGUMENTS}
	--host $<TARGET_FILE:fex_modhost_substrate> --abi $<TARGET_FILE:fex_modhost_abi>)
set_tests_properties(fex_modhost_execution PROPERTIES TIMEOUT 60 LABELS "fex;execution" SKIP_RETURN_CODE 77)

add_test(NAME fex_modhost_tier0_gap_expected_failure COMMAND ${Python3_EXECUTABLE}
	"${RAVEN_MODHOST_REPO}/scripts/test-fex-modhost.py" --tier0-gap
	--host $<TARGET_FILE:fex_modhost_substrate> --abi $<TARGET_FILE:fex_modhost_abi>)
set_tests_properties(fex_modhost_tier0_gap_expected_failure PROPERTIES TIMEOUT 30 WILL_FAIL TRUE LABELS "fex;expected-failure")

# M4 probes are kept here to avoid touching the shared Darwin option gating.
add_executable(fex_guest_window_test ../guest_window_test.cpp)
target_compile_features(fex_guest_window_test PRIVATE cxx_std_17)
add_test(NAME native_guest_window_fex COMMAND fex_guest_window_test)
set_tests_properties(native_guest_window_fex PROPERTIES TIMEOUT 15 LABELS "fex;m4;placement")
include(${CMAKE_CURRENT_LIST_DIR}/../m4.cmake)
