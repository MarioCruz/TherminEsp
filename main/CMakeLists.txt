idf_component_register(
    SRCS "main.c" "synth.c" "ui.c" "camera.c" "tracker.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES "testhand_224.rgb"
)
