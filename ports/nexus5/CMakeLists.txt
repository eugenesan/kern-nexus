cmake_minimum_required(VERSION 3.16)
project(kern_nexus C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Experimental: compile camd into the kern binary instead of spawning the
# standalone daemon.  Default OFF; the external daemon remains the shipped
# behaviour.  Stage 0 only adds the flag and the shim/log ring, so enabling
# it does not yet change any runtime path.
option(KERN_CAMERA_IN_PROCESS
       "Fold camd into kern (experimental, default OFF)" OFF)

set(KERN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../kern")
set(PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(SIM_PLATFORM "${KERN_DIR}/simulator/platform")

if(NOT EXISTS "${KERN_DIR}/main/main.c")
  message(FATAL_ERROR "kern/ is not checked out at ${KERN_DIR}")
endif()

# Offline vendored LVGL 9.5 (no FetchContent).
set(LVGL_DIR "${KERN_DIR}/third_party/lvgl" CACHE PATH "LVGL 9.5 source")
if(NOT EXISTS "${LVGL_DIR}/src/lvgl.h")
  message(FATAL_ERROR
    "LVGL 9.5 not found at ${LVGL_DIR}.  Check out the kern submodules "
    "or pass -DLVGL_DIR=/path/to/lvgl.")
endif()

# ARM static mbedTLS (build with scripts/build-mbedtls.sh).
set(MBEDTLS_ROOT "$ENV{MBEDTLS_ROOT}" CACHE PATH "mbedTLS install prefix")
if(NOT MBEDTLS_ROOT)
  set(MBEDTLS_ROOT "$ENV{KERN_NEXUS_DEPS}/mbedtls-armhf")
endif()
if(NOT MBEDTLS_ROOT)
  set(MBEDTLS_ROOT "/tmp/kern-nexus/artifacts/deps/mbedtls-armhf")
endif()
# The toolchain restricts library search to the sysroot; the vendored
# mbedTLS is outside it, so look explicitly first.
if(EXISTS "${MBEDTLS_ROOT}/include/mbedtls/pkcs5.h")
  set(MBEDTLS_INCLUDE_DIR "${MBEDTLS_ROOT}/include")
endif()
foreach(_mbed_lib mbedtls mbedcrypto mbedx509)
  if(EXISTS "${MBEDTLS_ROOT}/lib/lib${_mbed_lib}.a")
    set(_MBED_${_mbed_lib}_LIB "${MBEDTLS_ROOT}/lib/lib${_mbed_lib}.a")
  endif()
endforeach()
if(NOT MBEDTLS_INCLUDE_DIR)
  find_path(MBEDTLS_INCLUDE_DIR mbedtls/pkcs5.h HINTS "${MBEDTLS_ROOT}/include")
endif()
if(NOT _MBED_mbedtls_LIB)
  find_library(_MBED_mbedtls_LIB mbedtls HINTS "${MBEDTLS_ROOT}/lib")
endif()
if(NOT _MBED_mbedcrypto_LIB)
  find_library(_MBED_mbedcrypto_LIB mbedcrypto HINTS "${MBEDTLS_ROOT}/lib")
endif()
if(NOT _MBED_mbedx509_LIB)
  find_library(_MBED_mbedx509_LIB mbedx509 HINTS "${MBEDTLS_ROOT}/lib")
endif()
if(NOT MBEDTLS_INCLUDE_DIR OR NOT _MBED_mbedtls_LIB OR NOT _MBED_mbedcrypto_LIB)
  message(FATAL_ERROR
    "ARM mbedTLS not found under ${MBEDTLS_ROOT}.  "
    "Run scripts/build-mbedtls.sh or set MBEDTLS_ROOT.")
endif()
set(MBEDTLS_LIB "${_MBED_mbedtls_LIB}")
set(MBEDCRYPTO_LIB "${_MBED_mbedcrypto_LIB}")
set(MBEDX509_LIB "${_MBED_mbedx509_LIB}")

# Vendored static zlib (the ARM sysroot has none).
set(ZLIB_ROOT "$ENV{ZLIB_ROOT}" CACHE PATH "zlib install prefix")
if(NOT ZLIB_ROOT)
  set(ZLIB_ROOT "$ENV{KERN_NEXUS_DEPS}/zlib-armhf")
endif()
if(NOT ZLIB_ROOT)
  set(ZLIB_ROOT "/tmp/kern-nexus/artifacts/deps/zlib-armhf")
endif()
if(EXISTS "${ZLIB_ROOT}/include/zlib.h")
  set(ZLIB_INCLUDE_DIR "${ZLIB_ROOT}/include")
endif()
if(EXISTS "${ZLIB_ROOT}/lib/libz.a")
  set(Z_LIB "${ZLIB_ROOT}/lib/libz.a")
endif()
if(NOT ZLIB_INCLUDE_DIR)
  find_path(ZLIB_INCLUDE_DIR zlib.h HINTS "${ZLIB_ROOT}/include")
endif()
if(NOT Z_LIB)
  find_library(Z_LIB z HINTS "${ZLIB_ROOT}/lib")
endif()
if(NOT ZLIB_INCLUDE_DIR OR NOT Z_LIB)
  message(FATAL_ERROR
    "static zlib not found under ${ZLIB_ROOT}.  Run scripts/build-zlib.sh.")
endif()
find_package(Threads REQUIRED)

# Application: every main/**/*.c except host tests and the ESP-only
# storage/pbkdf2/fw_update implementations (the simulator stubs replace them).
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS "${KERN_DIR}/main/*.c")
list(FILTER APP_SOURCES EXCLUDE REGEX "/test/")
list(FILTER APP_SOURCES EXCLUDE REGEX "/main/core/storage\\.c$")
list(FILTER APP_SOURCES EXCLUDE REGEX "/main/core/pbkdf2\\.c$")
list(FILTER APP_SOURCES EXCLUDE REGEX "/main/core/pbkdf2_selftest\\.c$")
list(FILTER APP_SOURCES EXCLUDE REGEX "/main/core/fw_update\\.c$")

# Components that are portable C.  ESP video/sd_card/BSP components are not
# linked; the port layer replaces them.
file(GLOB WALLY_SOURCES "${KERN_DIR}/components/libwally-core/kern_wally.c"
                        "${KERN_DIR}/components/libwally-core/upstream/src/amalgamation/combined.c")
file(GLOB_RECURSE CUR_SOURCES "${KERN_DIR}/components/cUR/src/*.c")
file(GLOB QUIRC_SOURCES "${KERN_DIR}/components/k_quirc/src/*.c")
file(GLOB BBQR_SOURCES "${KERN_DIR}/components/bbqr/src/*.c")
file(GLOB DEFLATE_SOURCES "${KERN_DIR}/components/deflate_codec/src/*.c")
file(GLOB BIP138_SOURCES "${KERN_DIR}/components/bip138/src/*.c")
set(BUS_SOURCES "${KERN_DIR}/components/bus_timeout/bus_timeout.c")
set(SECURE_SOURCES
    "${KERN_DIR}/components/secure_memory/secure_memory.c"
    "${KERN_DIR}/components/secure_memory/secret_alloc.c")

file(GLOB PORT_SOURCES "${PORT_DIR}/platform/*.c")
file(GLOB CAMERA_SOURCES "${PORT_DIR}/../../components/camera/*.c")
# camd.c is the reference daemon; it is compiled only after the refactor.
list(FILTER CAMERA_SOURCES EXCLUDE REGEX "/camd\\.c$")
file(GLOB NFC_SOURCES "${PORT_DIR}/../../components/nfc/*.c")
file(GLOB SEEDKEEPER_SOURCES "${PORT_DIR}/../../components/seedkeeper/*.c")

set(SIM_STUB_SOURCES
    "${SIM_PLATFORM}/esp_idf_stubs/stubs.c"
    "${SIM_PLATFORM}/esp_idf_stubs/freertos_sim.c"
    "${SIM_PLATFORM}/esp_idf_stubs/nvs_sim.c"
    "${SIM_PLATFORM}/esp_idf_stubs/sim_flash.c"
    "${SIM_PLATFORM}/esp_idf_stubs/sim_storage_hooks.c"
    "${SIM_PLATFORM}/esp_idf_stubs/efuse_hmac_sim.c"
    "${SIM_PLATFORM}/esp_idf_stubs/fw_update_sim.c"
    "${SIM_PLATFORM}/esp_idf_stubs/pbkdf2_sim.c"
    "${SIM_PLATFORM}/esp_idf_stubs/storage_sim.c"
    "${SIM_PLATFORM}/sd_card_sim/sd_card_sim.c")

file(GLOB_RECURSE LVGL_SOURCES "${LVGL_DIR}/src/*.c")

add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl PUBLIC "${LVGL_DIR}" "${LVGL_DIR}/src")
target_compile_definitions(lvgl PRIVATE LV_CONF_INCLUDE_SIMPLE)
target_include_directories(lvgl PRIVATE "${PORT_DIR}")
target_link_libraries(lvgl PUBLIC m)

add_executable(kern-nexus
    ${APP_SOURCES}
    ${WALLY_SOURCES}
    ${CUR_SOURCES}
    ${QUIRC_SOURCES}
    ${BBQR_SOURCES}
    ${DEFLATE_SOURCES}
    ${BIP138_SOURCES}
    ${BUS_SOURCES}
    ${SECURE_SOURCES}
    ${PORT_SOURCES}
    ${CAMERA_SOURCES}
    ${NFC_SOURCES}
    ${SEEDKEEPER_SOURCES}
    ${SIM_STUB_SOURCES}
)

target_include_directories(kern-nexus PRIVATE
    "${PORT_DIR}/include"
    "${PORT_DIR}"
    "${SIM_PLATFORM}/esp_idf_stubs/include"
    "${SIM_PLATFORM}/bsp_sim/include"
    "${SIM_PLATFORM}/video_sim/include"
    "${SIM_PLATFORM}/sd_card_sim/include"
    "${SIM_PLATFORM}"
    "${KERN_DIR}/main"
    "${KERN_DIR}/components"
    "${KERN_DIR}/components/libwally-core"
    "${KERN_DIR}/components/libwally-core/upstream"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/seedkeeper"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/nfc"
    "${KERN_DIR}/components/libwally-core/upstream/include"
    "${KERN_DIR}/components/libwally-core/upstream/src"
    "${KERN_DIR}/components/libwally-core/upstream/src/ccan"
    "${KERN_DIR}/components/libwally-core/upstream/src/secp256k1"
    "${KERN_DIR}/components/libwally-core/upstream/src/secp256k1/include"
    "${KERN_DIR}/components/libwally-core/upstream/src/secp256k1/src"
    "${KERN_DIR}/components/cUR/src"
    "${KERN_DIR}/components/k_quirc/include"
    "${KERN_DIR}/components/k_quirc/src"
    "${KERN_DIR}/components/bbqr/src"
    "${KERN_DIR}/components/bip138/include"
    "${KERN_DIR}/components/secure_memory/include"
    "${KERN_DIR}/components/bus_timeout/include"
    "${KERN_DIR}/components/deflate_codec/src"
    "${KERN_DIR}/components/video"
    "${KERN_DIR}/components/bsp_common/include"
    "${MBEDTLS_INCLUDE_DIR}"
    "${ZLIB_INCLUDE_DIR}"
    "${LVGL_DIR}"
    "${LVGL_DIR}/src"
)

target_compile_definitions(kern-nexus PRIVATE
    LV_CONF_INCLUDE_SIMPLE
    KERN_NEXUS=1
    SIM_LCD_H_RES=1080
    SIM_LCD_V_RES=1920
    K_QUIRC_ADAPTIVE_THRESHOLD
    K_QUIRC_BILINEAR_THRESHOLD
    WALLY_ABI_NO_ELEMENTS
    BUILD_MINIMAL=1
    BUILD_STANDARD_SECP=1
    $<$<BOOL:${KERN_CAMERA_IN_PROCESS}>:KERN_CAMERA_IN_PROCESS=1>
    ECMULT_WINDOW_SIZE=8
    # Linux 3.4 has no getrandom(2).
    HAVE_GETRANDOM=0
)

target_compile_options(kern-nexus PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
    -Wno-sign-compare -Wno-missing-field-initializers
    -fstack-protector-strong -D_FORTIFY_SOURCE=2
    -flax-vector-conversions
)

target_link_libraries(kern-nexus PRIVATE
    lvgl
    ${MBEDTLS_LIB} ${MBEDCRYPTO_LIB} ${MBEDX509_LIB}
    ${Z_LIB}
    Threads::Threads
    m
)

set_target_properties(kern-nexus PROPERTIES
    OUTPUT_NAME kern-nexus
    LINK_FLAGS "-static -Wl,--gc-sections -Wl,--wrap=free -Wl,--wrap=realloc"
)
