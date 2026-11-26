cmake_minimum_required(VERSION 3.16)

project(onekvm_video_rk_mpp LANGUAGES C)

option(BUILD_TESTING "Build host-side unit tests" OFF)
option(ONEKVM_HAVE_ROCKIT "Link Rockchip MPI (librockit) for VENC" OFF)
set(ROCKIT_ROOT "" CACHE PATH "Rockchip MPI sysroot with include/ and lib/")

include(CTest)
include(GNUInstallDirs)
find_package(Threads REQUIRED)

function(onekvm_enable_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-Wall>
        $<$<COMPILE_LANGUAGE:C>:-Wextra>
        $<$<COMPILE_LANGUAGE:C>:-Wpedantic>)
endfunction()

add_library(rk_mpp SHARED
    src/onekvm_video_backend.c
    src/onekvm_crypto_backend.c
    src/rk_mpp_encoder.c
    src/no_signal.c
    src/rga_scale.c
    src/snapshot.c
    src/edid_store.c
    src/v4l2_mplane.c)

target_include_directories(rk_mpp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(rk_mpp PRIVATE c_std_11)
target_compile_definitions(rk_mpp PRIVATE _GNU_SOURCE)
onekvm_enable_warnings(rk_mpp)
target_link_libraries(rk_mpp PRIVATE Threads::Threads)
target_link_options(rk_mpp PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/onekvm_backend.map")
set_property(TARGET rk_mpp APPEND PROPERTY
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/onekvm_backend.map")

if(ONEKVM_HAVE_ROCKIT)
    if(NOT ROCKIT_ROOT)
        message(FATAL_ERROR "ROCKIT_ROOT must point at a Rockchip MPI sysroot")
    endif()
    target_compile_definitions(rk_mpp PRIVATE ONEKVM_HAVE_ROCKIT)
    target_include_directories(rk_mpp PRIVATE "${ROCKIT_ROOT}/include")
    target_link_directories(rk_mpp PRIVATE "${ROCKIT_ROOT}/lib")
    target_link_libraries(rk_mpp PRIVATE rockit)
endif()

set_target_properties(rk_mpp PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX ""
    OUTPUT_NAME "rk-mpp"
    POSITION_INDEPENDENT_CODE ON)

set(ONEKVM_VIDEO_BACKEND_INSTALL_DIR
    "${CMAKE_INSTALL_LIBDIR}/onekvm/video-backends"
    CACHE STRING "Install directory for OneKVM video backend DSOs")
install(TARGETS rk_mpp
    LIBRARY DESTINATION "${ONEKVM_VIDEO_BACKEND_INSTALL_DIR}")
install(FILES packaging/onekvm/picokvm-device-backend.json
    DESTINATION "${CMAKE_INSTALL_DATADIR}/onekvm/device-backends"
    RENAME picokvm.json)
install(FILES packaging/onekvm/picokvm-system-plugin.json
    DESTINATION "${CMAKE_INSTALL_DATADIR}/onekvm/system-plugins"
    RENAME picokvm-device.json)
install(FILES include/onekvm/video_backend_v1.h
              include/onekvm/crypto_backend_v1.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/onekvm")

if(BUILD_TESTING)
    add_executable(au-queue-test tests/au-queue-test.c src/v4l2_mplane.c
        src/rga_scale.c src/no_signal.c src/snapshot.c src/edid_store.c)
    target_include_directories(au-queue-test PRIVATE include src)
    target_compile_definitions(au-queue-test PRIVATE _GNU_SOURCE)
    onekvm_enable_warnings(au-queue-test)
    target_link_libraries(au-queue-test PRIVATE Threads::Threads)
    add_test(NAME au-queue COMMAND au-queue-test)

    add_executable(scale-recovery-test tests/scale-recovery-test.c src/no_signal.c)
    target_include_directories(scale-recovery-test PRIVATE include src)
    target_compile_definitions(scale-recovery-test PRIVATE _GNU_SOURCE)
    onekvm_enable_warnings(scale-recovery-test)
    target_link_libraries(scale-recovery-test PRIVATE Threads::Threads)
    add_test(NAME scale-recovery COMMAND scale-recovery-test)

    add_executable(packet-backpressure-test tests/packet-backpressure-test.c src/v4l2_mplane.c
        src/rga_scale.c src/no_signal.c src/edid_store.c)
    target_include_directories(packet-backpressure-test PRIVATE include src)
    target_compile_definitions(packet-backpressure-test PRIVATE _GNU_SOURCE)
    onekvm_enable_warnings(packet-backpressure-test)
    target_link_libraries(packet-backpressure-test PRIVATE Threads::Threads)
    add_test(NAME packet-backpressure COMMAND packet-backpressure-test)

    add_executable(snapshot-test tests/snapshot-test.c)
    target_include_directories(snapshot-test PRIVATE include src)
    target_compile_definitions(snapshot-test PRIVATE _GNU_SOURCE)
    onekvm_enable_warnings(snapshot-test)
    target_link_libraries(snapshot-test PRIVATE Threads::Threads)
    add_test(NAME snapshot COMMAND snapshot-test)

    add_executable(source-reset-test tests/source-reset-test.c src/rga_scale.c
        src/snapshot.c src/rk_mpp_encoder.c src/no_signal.c src/edid_store.c)
    target_include_directories(source-reset-test PRIVATE include src)
    target_compile_definitions(source-reset-test PRIVATE _GNU_SOURCE)
    onekvm_enable_warnings(source-reset-test)
    target_link_libraries(source-reset-test PRIVATE Threads::Threads)
    add_test(NAME source-reset COMMAND source-reset-test)

    add_executable(rga-dma-test tests/rga-dma-test.c)
    target_include_directories(rga-dma-test PRIVATE include src)
    target_compile_definitions(rga-dma-test PRIVATE _GNU_SOURCE)
    onekvm_enable_warnings(rga-dma-test)
    add_test(NAME rga-dma COMMAND rga-dma-test)

    add_executable(rga-display-release-test tests/rga-display-release-test.c)
    target_include_directories(rga-display-release-test PRIVATE include src)
    target_compile_definitions(rga-display-release-test PRIVATE _GNU_SOURCE)
    onekvm_enable_warnings(rga-display-release-test)
    add_test(NAME rga-display-release COMMAND rga-display-release-test)

    add_executable(rate-update-test tests/rate-update-test.c src/v4l2_mplane.c
        src/rga_scale.c src/no_signal.c src/snapshot.c src/edid_store.c)
    target_include_directories(rate-update-test PRIVATE include src)
    target_compile_definitions(rate-update-test PRIVATE _GNU_SOURCE)
    onekvm_enable_warnings(rate-update-test)
    target_link_libraries(rate-update-test PRIVATE Threads::Threads)
    add_test(NAME rate-update COMMAND rate-update-test)


    add_executable(abi-smoke tests/abi-smoke.c)
    target_include_directories(abi-smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(abi-smoke PRIVATE c_std_11)
    target_compile_definitions(abi-smoke PRIVATE _GNU_SOURCE)
    onekvm_enable_warnings(abi-smoke)
    target_link_libraries(abi-smoke PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(abi-smoke rk_mpp)
    add_test(NAME abi-smoke COMMAND abi-smoke $<TARGET_FILE:rk_mpp>)

    add_executable(no-signal-test tests/no-signal-test.c src/no_signal.c)
    target_include_directories(no-signal-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(no-signal-test PRIVATE c_std_11)
    target_compile_definitions(no-signal-test PRIVATE _GNU_SOURCE)
    onekvm_enable_warnings(no-signal-test)
    add_test(NAME no-signal COMMAND no-signal-test)

    add_executable(display-csc-test tests/display-csc-test.c)
    target_include_directories(display-csc-test PRIVATE include src)
    onekvm_enable_warnings(display-csc-test)
    target_link_libraries(display-csc-test PRIVATE m)
    add_test(NAME display-csc COMMAND display-csc-test)

    add_executable(edid-store-test tests/edid-store-test.c src/edid_store.c)
    target_include_directories(edid-store-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(edid-store-test PRIVATE c_std_11)
    target_compile_definitions(edid-store-test PRIVATE _GNU_SOURCE)
    onekvm_enable_warnings(edid-store-test)
    add_test(NAME edid-store COMMAND edid-store-test)
endif()
