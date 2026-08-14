idf_component_register(
    SRCS "main.c" "synth.c" "ui.c" "camera.c" "tracker.cpp" "settings.c" "buttons.c" "recorder.c"
    INCLUDE_DIRS "."
    EMBED_FILES "testhand_224.rgb"
    PRIV_REQUIRES "nvs_flash"
)
