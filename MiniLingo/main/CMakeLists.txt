# Hinweis: die *_test.c-Dateien in diesem Verzeichnis sind die
# Hardware-Komponententests (Phase 0, siehe COMPONENT_TESTS.md) und
# werden nicht mitgebaut — sie dienen als Referenz.
idf_component_register(
    SRCS "main.c"
         "drivers/display.c"
         "drivers/touch.c"
         "network/wifi.c"
         "ui/ui.c"
         "ui/ui_status_bar.c"
         "ui/ui_chat.c"
         "ui/ui_mic_button.c"
    INCLUDE_DIRS ".")
