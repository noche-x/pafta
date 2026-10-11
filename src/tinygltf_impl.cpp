// tinygltf's implementation includes Windows.h. Keep it out of the raylib
// translation unit, where Windows API names conflict with raylib names.
// raylib provides the stb symbols; the exporter writes images through raylib.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include "tiny_gltf.h"
