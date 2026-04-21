idf_component_register(SRCS "encevB35.c" "main.c" "wavplayer.c" "cserial.c" "pevent.c" "fupdate.c" "encevW11.c" "encevG80.c" "cmdapi.c" "usbserial.c"
                       INCLUDE_DIRS "."
                       REQUIRES fatfs esp_driver_gptimer esp_driver_sdspi esp_driver_uart app_update esp_app_format esp_driver_i2c
                       )
