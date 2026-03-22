cmake_minimum_required(VERSION 3.16.3)
project(ascend310b_dtbtool)

add_subdirectory(src)

set(DTS_BASE_DIR ${TOP_DIR}/dtb/dts)
if(PRODUCT STREQUAL "ascend310Besl" OR
   PRODUCT STREQUAL "ascend310B" OR
   PRODUCT STREQUAL "ascend310Bemu" OR
   PRODUCT STREQUAL "ascend310Brc" OR
   PRODUCT STREQUAL "ascend310Brcesl" OR
   PRODUCT STREQUAL "ascend310Brcemu")
    # 当前driver跟esl用相同的dts，后期需要更新到单独的目录
    set(DTS_SRC_DIR ${DTS_BASE_DIR}/hi1910b/hi1910BL)
    set(DTS_FILE_LIST hi1910B-orangepiaipro20t.dts hi1910B-orangepiaipro8t.dts)
    set(OUT_DT_IMAGE dt.img)
else()
    message(FATAL_ERROR "unsupported chip '${PRODUCT}' for Ascend310B dtb target")
endif()

# 编译单个dtb文件
set(DTB_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/dtbs)
add_custom_command(OUTPUT ${DTB_OUT_DIR}
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/mkdtb.sh ${DTB_OUT_DIR} ${DTS_SRC_DIR} ${DTS_FILE_LIST}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/mkdtb.sh
)
add_custom_target(dtb_single_files DEPENDS ${DTB_OUT_DIR})

# 编译dt.img
set(OUT_DT_IMAGE ${CMAKE_CURRENT_BINARY_DIR}/${OUT_DT_IMAGE})
add_custom_command(OUTPUT ${OUT_DT_IMAGE}
    COMMAND ${BIN_DTBTOOL} -o ${OUT_DT_IMAGE} ${DTB_OUT_DIR}/
    DEPENDS private_target_dtbtool dtb_single_files
)
add_custom_target(device_dtb DEPENDS ${OUT_DT_IMAGE})

install(DIRECTORY ${DTB_OUT_DIR}/
    DESTINATION bin
    OPTIONAL
)

install(FILES ${OUT_DT_IMAGE} ${dtb_single_files}
    DESTINATION bin
    OPTIONAL
)
