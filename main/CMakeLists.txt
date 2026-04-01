idf_component_register(SRCS "main.c" "wavplayer.c" "cserial.c" "pevent.c" "fupdate.c" "encevW11.c" "encevG80.c"
                       INCLUDE_DIRS "."
                       REQUIRES fatfs esp_driver_gptimer esp_driver_sdspi esp_driver_uart app_update esp_app_format esp_driver_i2c
                       )
