// SPDX-License-Identifier: GPL-2.0-only
// tinygltf's implementation includes Windows.h. Keep it out of the raylib source
// file, because Windows API names clash with raylib names.
// raylib provides the stb symbols, and the exporter writes images through raylib.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include "tiny_gltf.h"
