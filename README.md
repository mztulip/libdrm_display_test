# libdrm_display_test
Simple program to show color on displays uing libdrm. Basic example for testing DRM KMS driver.


Compile:
gcc main.c -I/usr/include/libdrm -ldrm

Materials used to prepare this code:
Modetest application source: https://github.com/grate-driver/libdrm/blob/master/tests/modetest/modetest.c
Great short description how KMS works, with associated example: 
https://embear.ch/blog/drm-framebuffer
https://github.com/embear-engineering/drm-framebuffer

Another simple examples:
https://github.com/ascent12/drm_doc