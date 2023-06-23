idf_component_register(SRCS "webserver.c" "wifi.c" "blink.c" "main.c"
                    INCLUDE_DIRS ".")
spiffs_create_partition_image(storage ../pages FLASH_IN_PROJECT)
