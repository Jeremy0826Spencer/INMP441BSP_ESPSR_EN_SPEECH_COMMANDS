set(SKAINET_HW
    "${CMAKE_CURRENT_LIST_DIR}/../../../../components/hardware_driver"
)

idf_component_register(
    SRCS
        "bsp_board.c"
        "${SKAINET_HW}/esp_board_init.c"
    INCLUDE_DIRS
        "${SKAINET_HW}/include"
        "${SKAINET_HW}/boards/include"
    REQUIRES
        esp_driver_i2s
        esp_driver_gpio
        fatfs
        sdmmc
)