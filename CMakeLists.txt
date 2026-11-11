idf_component_register(SRCS "src/pluto_iiod.c" "src/pluto_iq.c" "src/pluto_plan.c" "src/pluto_usb.c" "src/pluto_sdr.c" INCLUDE_DIRS "include" PRIV_REQUIRES usb esp_timer heap)
