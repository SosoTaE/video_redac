/*
 * stb_image_impl.c — stb_image-ის ერთადერთი translation unit.
 *
 * header-only ბიბლიოთეკის იმპლემენტაცია ცალკე ფაილშია გატანილი ორი მიზეზით:
 *   1. ის ერთხელ კომპილირდება და აღარ ანელებს media_loader.c-ის ბილდს;
 *   2. მისი (მრავალრიცხოვანი) warning-ები აქვე იხშობა და პროექტის საკუთარ
 *      -Wall -Wextra-ს არ ჩრდილავს.
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_WARNING
/* გამორთული ფორმატები — ვამცირებთ ბინარის ზომას და attack surface-ს. */
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#include "stb_image.h"

#pragma GCC diagnostic pop
