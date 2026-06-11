# Hinweis: die *_test.c-Dateien in diesem Verzeichnis sind die
# Hardware-Komponententests (Phase 0, siehe COMPONENT_TESTS.md) und
# werden nicht mitgebaut — sie dienen als Referenz.
idf_component_register(
    SRCS "main.c"
         "state.c"
         "drivers/display.c"
         "drivers/touch.c"
         "drivers/audio_in.c"
         "drivers/audio_out.c"
         "local/canned_audio.c"
         "local/timer.c"
         "local/volume.c"
         "network/wifi.c"
         "network/cloud.c"
         "network/http_util.c"
         "network/whisper.c"
         "network/gemini.c"
         "debug/wav_dump.c"
         "speech/afe_pipeline.c"
         "speech/commands.c"
         "speech/recorder.c"
         "speech/wake_word.c"
         "speech/wav.c"
         "ui/ui.c"
         "ui/ui_status_bar.c"
         "ui/ui_chat.c"
         "ui/ui_mic_button.c"
    INCLUDE_DIRS ".")

# Canned-TTS-WAVs (scripts/generate_canned_tts.py -> spiffs/) als
# SPIFFS-Image in die storage-Partition; wird bei "idf.py flash"
# mitgeflasht.
spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
