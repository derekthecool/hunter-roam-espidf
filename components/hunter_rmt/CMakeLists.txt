idf_component_register(
    SRCS "hunter_rmt.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_rmt esp_driver_gpio
)
