/*
 * stb_image_impl.c — the single translation unit for stb_image.
 *
 * The header-only library's implementation is isolated here for two reasons:
 *   1. it compiles once and no longer slows down media_loader.c;
 *   2. its (numerous) warnings are silenced here instead of drowning out the
 *      project's own -Wall -Wextra.
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_WARNING
/* Disabled formats — smaller binary and a smaller attack surface. */
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#include "stb_image.h"

#pragma GCC diagnostic pop
