idf_component_register(SRCS "if_B35.c" "main.c" "wavplayer.c" "serial.c" "if_flat.c" "fupdate.c" "if_sys11.c" "if_G80.c" "cmdapi.c" "usbserial.c" "wifi.c" "httpserver.c"
                       INCLUDE_DIRS "."
                       REQUIRES fatfs esp_driver_gptimer esp_driver_sdspi esp_driver_uart app_update esp_app_format esp_driver_i2c
                                esp_wifi esp_netif esp_event esp_http_server nvs_flash lwip
                       )
