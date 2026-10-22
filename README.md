# BullyMSH iOS / Android

A header-only C++17 library for reading and writing mesh files used by **Bully: Anniversary Edition** (`.msh`) and Wavefront OBJ (`.obj`). It also includes helpers for mesh validation, bone hierarchies, and skin weights.
The library works with mesh data and file conversion. It does not render models or provide a model-editor interface.

It's a fully reverse-engineered using Frida, Bully AE v1.0.0.18 game crashes, and LLM (AI)

## Features

- Parse MSH and OBJ files
- Export mesh data to MSH or OBJ
- Read and write vertex positions, normals, tangents, UVs, colors, material slots, and skin weights
- Generate normals and tangents when importing OBJ data that does not provide them
- Transfer skin weights and the skeleton setup from a donor mesh
- Search and inspect bone hierarchies
- Validate mesh indices, material slots, bone parents, and skin-weight bone indices
- Preserve hierarchy, collision, and setup data from a donor MSH

## Requirements

- A C++17 compiler.
- The C++ standard library; no additional libraries are required.

## Installation

Copy `BullyMSH_iOS_Android.h` into your project and include it where needed:

```cpp
#include "BullyMSH_iOS_Android.h"
```

## OBJ to MSH

```cpp
#include "BullyMSH_iOS_Android.h"
#include <iostream>
#include <string>

int main() {
    BullyMSH::MeshData mesh;
    std::string error;

    if (!BullyMSH::LoadObj("input.obj", mesh, error)) {
        std::cerr << "Failed to load OBJ: " << error << '\n';
        return 1;
    }

    BullyMSH::ExportOptions options;
    options.modelName = "my_model";

    if (!BullyMSH::SaveMsh("output.msh", mesh, options, error)) {
        std::cerr << "Failed to save MSH: " << error << '\n';
        return 1;
    }

    return 0;
}
```

Or see [sample/Convert.cpp](https://github.com/amakizu/BullyMSH_iOS_Android/blob/main/sample/Convert.cpp)

## Build

**GCC**

```sh
g++ -std=c++17 main.cpp -o msh_converter
```

**Clang**

```sh
clang++ -std=c++17 main.cpp -o msh_converter
```

**MSVC**

```bat
cl /std:c++17 /EHsc main.cpp
```

**CMake**

```cmake
target_include_directories(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
```

**Emscripten / WebAssembly**

```sh
em++ -std=c++17 -s WASM=1 main.cpp -o msh_converter.js
```

## API overview

### Loading

- `LoadObj(path, mesh, error, scale, flipV, keepAxes, reverseWinding)` — loads an OBJ mesh. The optional arguments control scale, vertical UV flipping, axis conversion, and triangle winding.
- `LoadMsh(path, mesh, error)` — loads a Bully AE MSH mesh.

### Exporting

- `SaveObj(path, mesh, error)` — writes an OBJ file.
- `SaveMsh(path, mesh, options, error)` — writes a version 6 Bully AE MSH file.

### Skin weights

- `TransferSkin(destination, donor)` — copies skin weights from the nearest donor vertices and transfers the donor skeleton and `infoTail`. Returns average and maximum vertex distances.
- `SetVertexSkin(vertex, boneIndices, weights)` — sets a vertex's four bone influences and normalizes the weights.
- `NormalizeBlendWeights(vertex)` — normalizes the four weights to a total of 255.
- `GetTotalBlendWeight(vertex)` — returns the total weight value.
- `BuildBoneUsageTable(mesh)` — counts vertex influences for each bone index.
- `CountVerticesUsingBone(mesh, boneIndex)` — counts vertices influenced by a specific bone.

### Bones

- `FindBone(mesh, name)` — searches by exact name.
- `FindBoneNoCase(mesh, name)` — searches by name without case sensitivity.
- `FindBoneChildren(mesh, boneIndex)` — lists a bone's direct children.
- `FindRootBones(mesh)` — lists bones without a parent.
- `BuildBoneOrder(mesh)` — returns a depth-first order starting from root bones.
- `GetBoneDepth(mesh, boneIndex)` — returns a bone's depth in the hierarchy.

### Validation and format helpers

- `ValidateMesh(mesh, error)` — checks mesh geometry, indices, material references, bone parents, and skin-weight bone indices.
- `GetInfoPadding(version)` — returns the padding size used after the bone table.
- `GetAttributeTypeName(type)` — returns the name of a vertex attribute type.
- `GetAttributeUsageName(usage)` — returns the name of a vertex attribute usage.
- `GetAttributeTypeSize(type)` — returns the serialized size of an attribute type.
- `ComputeVertexStride(attributes)` — calculates a vertex stride from attribute declarations.

## Data structures

- `MeshData` — mesh name, vertices, indices, materials, bones, bounds, and `infoTail`.
- `Vertex` — position, normal, tangent, UV, material slot, four blend weights, four bone indices, and vertex color.
- `Bone` — name, 3×4 matrix, and parent index.
- `ExportOptions` — options used when saving MSH files, including model and fallback material names.
- `SkinStats` — average and maximum distances reported by `TransferSkin`.

## Format notes

- MSH data is stored in little-endian byte order.
- The container header is 24 bytes and points to the info and model blocks.
- Strings use a 32-bit length prefix and are not null-terminated.
- The writer outputs MSH version 6 with a 44-byte vertex stride.
- Vertex indices are 16-bit; a mesh is limited to 65,535 vertices by this format and implementation.
- Triangle-strip primitive groups are not supported by the loader.

The version 6 vertex layout is:

| Offset | Data |
| --- | --- |
| `0x00` | `float3` position |
| `0x0C` | `uint8[4]` blend weights |
| `0x10` | `uint8[4]` blend indices |
| `0x14` | packed normal (`int8[4]`) |
| `0x18` | `float3` tangent |
| `0x24` | `uint8[4]` vertex color |
| `0x28` | `int16[2]` UV |

When `MeshData::infoTail` comes from a donor MSH, the writer preserves its hierarchy, collision, and physical setup data while updating the mesh bounds. A new skeleton generally needs a compatible donor hierarchy to work with the game's existing animations.
