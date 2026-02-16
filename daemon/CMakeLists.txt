cmake_minimum_required(VERSION 3.20)
project(khor-daemon LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(khor-daemon
  src/main.cpp
  src/metrics.cpp
  src/app/app.cpp
  src/app/config.cpp
  src/audio/engine.cpp
  src/bpf/collector.cpp
  src/engine/music.cpp
  src/engine/signals.cpp
  src/http/server.cpp
  src/midi/alsa_seq.cpp
  src/osc/osc.cpp
  src/util/json.cpp
  src/util/paths.cpp
)

target_include_directories(khor-daemon PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/../bpf
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)

set_source_files_properties(src/audio/engine.cpp PROPERTIES COMPILE_DEFINITIONS MINIAUDIO_IMPLEMENTATION)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED libbpf)

target_include_directories(khor-daemon PRIVATE ${LIBBPF_INCLUDE_DIRS})
target_link_libraries(khor-daemon PRIVATE ${LIBBPF_LIBRARIES} pthread)

# Optional: ALSA sequencer for MIDI output.
find_path(ALSA_INCLUDE_DIR NAMES alsa/asoundlib.h)
find_library(ALSA_LIBRARY NAMES asound)
if (ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
  target_include_directories(khor-daemon PRIVATE ${ALSA_INCLUDE_DIR})
  target_link_libraries(khor-daemon PRIVATE ${ALSA_LIBRARY})
  target_compile_definitions(khor-daemon PRIVATE KHOR_HAS_ALSA_SEQ=1)
else()
  message(STATUS "ALSA sequencer headers not found; building without MIDI support.")
endif()

# ---- Tests ----
enable_testing()
add_executable(khor-tests
  tests/test_main.cpp
  src/engine/music.cpp
  src/engine/signals.cpp
)
target_include_directories(khor-tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(khor-tests PRIVATE pthread)
add_test(NAME khor-tests COMMAND khor-tests)

# ---- eBPF build steps (Linux-only) ----
if (UNIX AND EXISTS "/sys/kernel/btf/vmlinux")
  find_program(BPFTOOL bpftool)
  find_program(CLANG clang)

  if (BPFTOOL AND CLANG)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)

    set(VMLINUX_H ${CMAKE_CURRENT_BINARY_DIR}/generated/vmlinux.h)
    set(BPF_OBJ ${CMAKE_CURRENT_BINARY_DIR}/generated/khor.bpf.o)
    set(BPF_SKEL ${CMAKE_CURRENT_BINARY_DIR}/generated/khor.skel.h)

    add_custom_command(
      OUTPUT ${VMLINUX_H}
      COMMAND bash -c "${BPFTOOL} btf dump file /sys/kernel/btf/vmlinux format c > ${VMLINUX_H}"
      VERBATIM
    )

    add_custom_command(
      OUTPUT ${BPF_OBJ}
      DEPENDS ${VMLINUX_H} ${CMAKE_CURRENT_SOURCE_DIR}/../bpf/khor.bpf.c ${CMAKE_CURRENT_SOURCE_DIR}/../bpf/khor.h
      COMMAND ${CLANG}
        -g -O2 -target bpf
        -D__TARGET_ARCH_x86
        -D__BPF__=1
        -I${CMAKE_CURRENT_BINARY_DIR}/generated
        -I/usr/include
        -I/usr/include/bpf
        -I${CMAKE_CURRENT_SOURCE_DIR}/../bpf
        -c ${CMAKE_CURRENT_SOURCE_DIR}/../bpf/khor.bpf.c
        -o ${BPF_OBJ}
      VERBATIM
    )

    add_custom_command(
      OUTPUT ${BPF_SKEL}
      DEPENDS ${BPF_OBJ}
      COMMAND bash -c "${BPFTOOL} gen skeleton ${BPF_OBJ} > ${BPF_SKEL}"
      VERBATIM
    )

    add_custom_target(khor-bpf ALL DEPENDS ${BPF_SKEL})
    add_dependencies(khor-daemon khor-bpf)
    target_compile_definitions(khor-daemon PRIVATE KHOR_HAS_BPF=1)
  else()
    message(WARNING "bpftool/clang not found; building without eBPF support (run scripts/wsl-check.sh).")
  endif()
else()
  message(WARNING "/sys/kernel/btf/vmlinux not found; building without eBPF support.")
endif()
