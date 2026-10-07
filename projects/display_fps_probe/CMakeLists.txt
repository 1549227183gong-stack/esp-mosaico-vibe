cmake_minimum_required(VERSION 3.16)
set(MOSAICO_UTILS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../submodule/esp-mosaico-utils")
set(MOSAICO_BSP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../submodule/esp-mosaico-bsp")
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.application.defaults")
endif()
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_compiler.cmake")

# --- Windows 下 gspc depfile 路径修复 ----------------------------------------
# gspc 0.6.1 在 Windows 上会把 depfile 中的资源路径写成 \\?\ 扩展长度路径，
# Ninja 会把它记为非法路径，导致下一次增量构建在启动阶段直接失败。
# 这里用同参数的 .cmd 包装器先运行真实 gspc，再就地修正 depfile；
# 问题细节与手工验证方法见 tools/gspc_depfile_fix.py 文件头。
if(WIN32 AND NOT MOSAICO_DISABLE_GSPC_DEPFILE_FIX)
    set(MOSAICO_GSPC_FIX_DIR "${CMAKE_BINARY_DIR}/gspc-fix")
    set(MOSAICO_GSPC_FIX_WRAPPER "${MOSAICO_GSPC_FIX_DIR}/gspc_depfile_fix.cmd")

    # 首次配置时 GSPC_EXECUTABLE 是真实编译器，后续配置读到的是包装器，
    # 因此用独立缓存变量保存真实路径，避免包装器递归调用自身。
    if(NOT MOSAICO_GSPC_REAL_EXECUTABLE)
        set(MOSAICO_GSPC_REAL_EXECUTABLE "${GSPC_EXECUTABLE}"
            CACHE FILEPATH "GSPC 真实可执行文件路径（未包装）" FORCE)
    endif()
    if(MOSAICO_GSPC_REAL_EXECUTABLE STREQUAL MOSAICO_GSPC_FIX_WRAPPER)
        message(FATAL_ERROR
            "MOSAICO_GSPC_REAL_EXECUTABLE 指向 depfile 修复包装器自身，"
            "请删除构建目录中的 CMakeCache.txt 后重新配置")
    endif()

    find_program(MOSAICO_GSPC_FIX_PYTHON NAMES python3 python REQUIRED)
    set(MOSAICO_GSPC_FIX_SCRIPT
        "${CMAKE_CURRENT_LIST_DIR}/tools/gspc_depfile_fix.py")
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/tools/gspc_depfile_fix.cmd.in"
        "${MOSAICO_GSPC_FIX_WRAPPER}" @ONLY)

    set(GSPC_EXECUTABLE "${MOSAICO_GSPC_FIX_WRAPPER}"
        CACHE FILEPATH "Pinned ESP-GSP compiler" FORCE)
    set(ENV{GSPC_EXECUTABLE} "${MOSAICO_GSPC_FIX_WRAPPER}")
elseif(MOSAICO_GSPC_REAL_EXECUTABLE)
    # 显式禁用修复时恢复真实编译器，避免缓存里残留上一次配置的包装器
    set(GSPC_EXECUTABLE "${MOSAICO_GSPC_REAL_EXECUTABLE}"
        CACHE FILEPATH "Pinned ESP-GSP compiler" FORCE)
    set(ENV{GSPC_EXECUTABLE} "${MOSAICO_GSPC_REAL_EXECUTABLE}")
endif()

list(APPEND EXTRA_COMPONENT_DIRS
    "${MOSAICO_UTILS_ROOT}/esp-mosaico-recovery/components/esp_mosaico_app_recovery"
    "${MOSAICO_UTILS_ROOT}/mosaico-tools/components/esp_mosaico_gsp_bundle"
    "${MOSAICO_UTILS_ROOT}/mosaico-tools/components/esp_mosaico_gsp_iris")
include("${MOSAICO_UTILS_ROOT}/esp-mosaico-recovery/cmake/mosaico_idf_project.cmake")
project(display_fps_probe VERSION 1.0.0)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/system_update.cmake")
