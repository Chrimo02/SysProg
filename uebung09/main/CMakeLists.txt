idf_component_register(
    SRCS "sd_card_picture.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver fatfs sdmmc esp_driver_sdspi
)