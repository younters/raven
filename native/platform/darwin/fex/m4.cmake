# SPDX-License-Identifier: MIT
# Included only by the opt-in module-host subtree. Shared Darwin defaults live elsewhere.
if(NOT RAVEN_MINGW32)
	message(WARNING "M4 WoW64 tests unavailable: set RAVEN_MINGW32 to an i686 MinGW compiler")
	return()
endif()
if(NOT TARGET raven_cpu)
	return()
endif()
set(M4_ENTRY ${CMAKE_BINARY_DIR}/native/fex-entry32.exe)
add_custom_command(OUTPUT ${M4_ENTRY}
	COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
		-Wl,--entry,_raven_entry,--image-base,0x400000,--no-insert-timestamp
		-o ${M4_ENTRY} ${RAVEN_MODHOST_REPO}/testdata/native_cpu/entry.c
	DEPENDS ${RAVEN_MODHOST_REPO}/testdata/native_cpu/entry.c VERBATIM)
set(M4_OUTPUTS ${M4_ENTRY})
add_test(NAME native_entry32_fex COMMAND ${Python3_EXECUTABLE} ${RAVEN_MODHOST_REPO}/scripts/test-fex-wow64.py
	--raven $<TARGET_FILE:raven> --fixture ${M4_ENTRY} --modules "${RAVEN_FEX_MODULE_ROOT}" --entry)
set_tests_properties(native_entry32_fex PROPERTIES TIMEOUT 30 LABELS "fex;m4;entry")
foreach(client ansi file threads)
	set(fixture ${CMAKE_BINARY_DIR}/native/kernel-${client}-fex32.exe)
	set(source ${RAVEN_MODHOST_REPO}/testdata/native_loader/kernel_${client}_client.c)
	if(client STREQUAL "file")
		set(source ${RAVEN_MODHOST_REPO}/testdata/native_loader/kernel_files_client.c)
	endif()
	add_custom_command(OUTPUT ${fixture}
		COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
			-Wl,--entry,_raven_entry,--image-base,0x400000,--no-insert-timestamp
			-o ${fixture} ${source} -lkernel32
		DEPENDS ${source} VERBATIM)
	list(APPEND M4_OUTPUTS ${fixture})
	add_test(NAME native_kernel_${client}32_fex COMMAND ${Python3_EXECUTABLE} ${RAVEN_MODHOST_REPO}/scripts/test-fex-wow64.py
		--raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}")
	set_tests_properties(native_kernel_${client}32_fex PROPERTIES TIMEOUT 30 LABELS "fex;m4;process")
endforeach()
add_custom_target(fex_m4_fixtures DEPENDS ${M4_OUTPUTS})
add_executable(fex_wow64_process_test ${CMAKE_CURRENT_LIST_DIR}/wow64_process_test.cpp)
target_link_libraries(fex_wow64_process_test PRIVATE raven_cpu)
# x18 holds the execution TEB; the macOS 11 legacy SDK permits its use.
target_link_options(fex_wow64_process_test PRIVATE -Wl,-platform_version,macos,11.0,11.0)
add_dependencies(fex_wow64_process_test fex_m4_fixtures)
foreach(contract fs fault timeout host-smc guest-smc stdcall cdecl)
	add_test(NAME native_wow64_${contract}_fex COMMAND fex_wow64_process_test ${M4_ENTRY} ${contract})
	set_tests_properties(native_wow64_${contract}_fex PROPERTIES TIMEOUT 15 LABELS "fex;m4;engine"
		ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
endforeach()
set(M4_SEH ${CMAKE_BINARY_DIR}/native/kernel-seh-fex32.exe)
add_custom_command(OUTPUT ${M4_SEH}
	COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
		-Wl,--entry,_raven_entry,--image-base,0x400000,--no-insert-timestamp
		-o ${M4_SEH} ${CMAKE_CURRENT_LIST_DIR}/wow64_seh_client.c -lkernel32
	DEPENDS ${CMAKE_CURRENT_LIST_DIR}/wow64_seh_client.c VERBATIM)
add_custom_target(fex_m4_seh_fixture DEPENDS ${M4_SEH})
add_test(NAME native_kernel_seh32_fex COMMAND ${Python3_EXECUTABLE} ${RAVEN_MODHOST_REPO}/scripts/test-fex-wow64.py
	--raven $<TARGET_FILE:raven> --fixture ${M4_SEH} --modules "${RAVEN_FEX_MODULE_ROOT}")
set_tests_properties(native_kernel_seh32_fex PROPERTIES TIMEOUT 30 LABELS "fex;m4;process;seh")
add_test(NAME native_kernel_threads32_parallel_fex COMMAND ${Python3_EXECUTABLE} ${RAVEN_MODHOST_REPO}/scripts/test-fex-wow64.py
	--raven $<TARGET_FILE:raven> --fixture ${CMAKE_BINARY_DIR}/native/kernel-threads-fex32.exe --modules "${RAVEN_FEX_MODULE_ROOT}" --parallel)
set_tests_properties(native_kernel_threads32_parallel_fex PROPERTIES TIMEOUT 30 LABELS "fex;m4;process;parallel")
add_dependencies(fex_wow64_process_test fex_m4_seh_fixture)
foreach(contract fault timeout host-smc guest-smc)
	add_test(NAME native_wow64_${contract}_parallel_fex COMMAND fex_wow64_process_test ${M4_ENTRY} ${contract})
	set_tests_properties(native_wow64_${contract}_parallel_fex PROPERTIES TIMEOUT 15 LABELS "fex;m4;engine;parallel"
		ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT};RAVEN_PARALLEL=1;RAVEN_SLICE_MS=0")
endforeach()

add_test(NAME native_wow64_native_brk_rejection_fex COMMAND ${Python3_EXECUTABLE} ${RAVEN_MODHOST_REPO}/scripts/test-fex-rejection.py
	--host $<TARGET_FILE:fex_wow64_process_test> --fixture ${M4_ENTRY} --modules "${RAVEN_FEX_MODULE_ROOT}" --wow64)
set_tests_properties(native_wow64_native_brk_rejection_fex PROPERTIES TIMEOUT 15 LABELS "fex;m4;fault")
add_test(NAME native_nt_service_fex COMMAND fex_process_test ${CMAKE_BINARY_DIR}/native/fex-entry.exe nt-service)
set_tests_properties(native_nt_service_fex PROPERTIES TIMEOUT 15 LABELS "fex;m4;contract"
	ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")

set(M4_ALIGN ${CMAKE_BINARY_DIR}/native/fex-alignment32.exe)
set(M4_CRT ${CMAKE_BINARY_DIR}/native/fex-crt32.exe)
if(RAVEN_DLLTOOL32)
	set(M4_DLLTOOL ${RAVEN_DLLTOOL32})
else()
	find_program(M4_DLLTOOL NAMES i686-w64-mingw32-dlltool llvm-dlltool REQUIRED)
endif()
set(M4_NTDLL_IMPORT ${CMAKE_BINARY_DIR}/native/fex-ntdll32.a)
add_custom_command(OUTPUT ${M4_NTDLL_IMPORT}
	COMMAND ${M4_DLLTOOL} -m i386 -d ${CMAKE_CURRENT_LIST_DIR}/wow64_crt_ntdll.def -l ${M4_NTDLL_IMPORT}
	DEPENDS ${CMAKE_CURRENT_LIST_DIR}/wow64_crt_ntdll.def VERBATIM)
add_custom_command(OUTPUT ${M4_ALIGN}
	COMMAND ${RAVEN_MINGW32} -nostdlib -Wl,--entry,_raven_entry,--no-insert-timestamp
		-o ${M4_ALIGN} ${CMAKE_CURRENT_LIST_DIR}/wow64_alignment.S
	DEPENDS ${CMAKE_CURRENT_LIST_DIR}/wow64_alignment.S VERBATIM)
add_custom_command(OUTPUT ${M4_CRT}
	COMMAND ${RAVEN_MINGW32} -nostdlib -Wl,--entry,_raven_entry,--no-insert-timestamp
		-o ${M4_CRT} ${CMAKE_CURRENT_LIST_DIR}/wow64_crt.S ${M4_NTDLL_IMPORT}
	DEPENDS ${CMAKE_CURRENT_LIST_DIR}/wow64_crt.S ${M4_NTDLL_IMPORT} VERBATIM)
add_custom_target(fex_m4_review_fixtures DEPENDS ${M4_ALIGN} ${M4_CRT})
add_dependencies(fex_wow64_process_test fex_m4_review_fixtures)
foreach(contract alignment range range-laa)
	set(fixture ${M4_ENTRY})
	if(contract STREQUAL "alignment")
		set(fixture ${M4_ALIGN})
	endif()
	add_test(NAME native_wow64_${contract}_fex COMMAND fex_wow64_process_test ${fixture} ${contract})
	set_tests_properties(native_wow64_${contract}_fex PROPERTIES TIMEOUT 15 LABELS "fex;m4;engine"
		ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
endforeach()
# Also run aligned SSE spills through the nested callback entry.
add_test(NAME native_wow64_alignment_callback_fex COMMAND fex_wow64_process_test ${M4_ALIGN} cdecl)
set_tests_properties(native_wow64_alignment_callback_fex PROPERTIES TIMEOUT 15 LABELS "fex;m4;engine"
	ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
add_executable(fex_wow64_crt_test ${CMAKE_CURRENT_LIST_DIR}/wow64_crt_test.cpp)
target_link_libraries(fex_wow64_crt_test PRIVATE raven_cpu)
target_link_options(fex_wow64_crt_test PRIVATE -Wl,-platform_version,macos,11.0,11.0)
add_dependencies(fex_wow64_crt_test fex_m4_review_fixtures raven-bootstrap)
foreach(engine native fex)
	add_test(NAME native_ntdll_cdecl_x32_${engine} COMMAND fex_wow64_crt_test $<TARGET_FILE:raven-bootstrap> ${M4_CRT} ${engine})
	set_tests_properties(native_ntdll_cdecl_x32_${engine} PROPERTIES TIMEOUT 30
		ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT};RAVEN_PREFIX=${CMAKE_BINARY_DIR}/m4-crt-${engine}-prefix")
endforeach()
set_tests_properties(native_ntdll_cdecl_x32_fex PROPERTIES LABELS "fex;m4;process")
add_executable(fex_wow64_boundary_test ${CMAKE_CURRENT_LIST_DIR}/wow64_boundary_test.cpp
	${CMAKE_CURRENT_LIST_DIR}/modhost/abi.S ${CMAKE_CURRENT_LIST_DIR}/modhost/context.S)
target_compile_features(fex_wow64_boundary_test PRIVATE cxx_std_17)
target_compile_options(fex_wow64_boundary_test PRIVATE -arch arm64 -ffixed-x18 -ffp-contract=off -Wall -Wextra -Werror -mmacosx-version-min=11.0)
target_compile_options(fex_wow64_boundary_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-UNDEBUG> -Wno-unused-function)
target_link_options(fex_wow64_boundary_test PRIVATE -arch arm64 -Wl,-platform_version,macos,11.0,11.0)
foreach(module wow64 x64)
	set(dll xtajit64.dll)
	if(module STREQUAL "wow64")
		set(dll xtajit.dll)
	endif()
	add_test(NAME native_${module}_host_av_rejection_fex COMMAND fex_wow64_boundary_test ${RAVEN_FEX_MODULE_ROOT}/${dll} ${module})
	set_tests_properties(native_${module}_host_av_rejection_fex PROPERTIES TIMEOUT 15 LABELS "fex;m4;fault")
endforeach()

foreach(laa 0 1)
	set(fixture ${CMAKE_BINARY_DIR}/native/fex-range32-${laa}.exe)
	set(laa_flag --disable-large-address-aware)
	if(laa)
		set(laa_flag --large-address-aware)
	endif()
	add_custom_command(OUTPUT ${fixture}
		COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe -DEXPECT_LAA=${laa}
			-Wl,--entry,_raven_entry,--image-base,0x90000000,--enable-reloc-section,--no-insert-timestamp,${laa_flag}
			-o ${fixture} ${CMAKE_CURRENT_LIST_DIR}/wow64_range.c -lkernel32
		DEPENDS ${CMAKE_CURRENT_LIST_DIR}/wow64_range.c VERBATIM)
	add_custom_target(fex_m4_range_${laa} DEPENDS ${fixture})
	add_dependencies(fex_wow64_process_test fex_m4_range_${laa})
	add_test(NAME native_wow64_range_process_${laa}_fex COMMAND ${Python3_EXECUTABLE} ${RAVEN_MODHOST_REPO}/scripts/test-fex-wow64.py
		--raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}")
	set_tests_properties(native_wow64_range_process_${laa}_fex PROPERTIES TIMEOUT 30 LABELS "fex;m4;process;placement")
endforeach()
# M4 is a required provisioning lane: missing modules must fail every M4 test.
set_property(TEST native_nt_service_fex APPEND PROPERTY ENVIRONMENT "RAVEN_FEX_REQUIRED=1")
