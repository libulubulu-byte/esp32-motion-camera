# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "")
  file(REMOVE_RECURSE
  "esp-idf\\esptool_py\\flasher_args.json.in"
  "esp-idf\\mbedtls\\x509_crt_bundle"
  "esp32s3_snapshot_kit.map"
  "flash_app_args"
  "flash_bootloader_args"
  "flasher_args.json"
  "human_face_detect.espdl.S"
  "index.html.S"
  "ldgen_libraries"
  "ldgen_libraries.in"
  "littlefs_py_venv"
  "project_elf_src_esp32s3.c"
  "x509_crt_bundle.S"
  )
endif()
