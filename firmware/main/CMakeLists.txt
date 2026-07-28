idf_component_register(SRCS      "app_main.cpp"
                                "app_settings.cpp"
                                "app_light.cpp"
                                "app_web.cpp"
                                "ld2420.cpp"
                      INCLUDE_DIRS "."
                      EMBED_FILES "www/index.html")

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
