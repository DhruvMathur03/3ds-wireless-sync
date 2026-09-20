/* Compiles the stb_image implementation exactly once. Restricted to
 * JPEG decoding only (this app only needs to preview .jpg/.jpeg files),
 * which keeps compiled size and decode complexity down. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "stb_image.h"
