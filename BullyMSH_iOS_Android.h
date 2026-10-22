#ifndef BULLYMSH_IOS_ANDROID
#define BULLYMSH_IOS_ANDROID

/**
 * @file BullyMSH_iOS_Android.h
 * @brief Bully Anniversary Edition MSH
 *
 * ============================================================================
 * OVERVIEW
 * ============================================================================
 * A single-header library for reading and writing Bully AE MSH and OBJ meshes.
 * It also provides mesh data structures, bone and skin-weight utilities, and
 * mesh validation.
 *
 * This header handles mesh data and file conversion. It does not render models
 * or provide an editor interface, so you might want to create your own project with this. 
 * NOTE - It's a fully reverse-engineered MSH format using frida, Bully AE v1.0.0.18 game crashes, and LLM (AI).
 *
 * ============================================================================
 * BUILDING
 * ============================================================================
 *   g++   : g++ -std=c++17 main.cpp -o main.cpp.out
 *   clang : clang++ -std=c++17 main.cpp -o main.cpp.out
 *   MSVC  : cl /std:c++17 /EHsc main.cpp
 *   CMake : target_include_directories(app PRIVATE <this folder>)
 *   WASM  : em++ -std=c++17 -s WASM=1 msh.cpp -o msh.js
 *
 * Requires only the C++ standard library. Load/save functions return a bool
 * and set an error message when an operation fails.
 *
 * ============================================================================
 * QUICK START
 * ============================================================================
 * @code
 *   #include "BullyMSH_iOS_Android.h"
 *
 *   BullyMSH::MeshData mesh;
 *   std::string error;
 *
 *   if (BullyMSH::LoadObj("model.obj", mesh, error)) {
 *       BullyMSH::ExportOptions options;
 *       options.modelName = "my_ped";
 *       BullyMSH::SaveMsh("model.msh", mesh, options, error);
 *   }
 * @endcode
 *
 * ============================================================================
 * PUBLIC API
 * ============================================================================
 *   Loading
 *     LoadObj(path, mesh, error, scale, flipV, keepAxes, reverseWinding)
 *     LoadMsh(path, mesh, error)
 *
 *   Writing
 *     SaveObj(path, mesh, error)
 *     SaveMsh(path, mesh, options, error)
 *
 *   Skinning
 *     TransferSkin(destination, donor)      -> SkinStats
 *     SetVertexSkin(vertex, boneIndices, weights)
 *     NormalizeBlendWeights(vertex)
 *     GetTotalBlendWeight(vertex)
 *     BuildBoneUsageTable(mesh)
 *     CountVerticesUsingBone(mesh, boneIndex)
 *
 *   Bones
 *     FindBone(mesh, name) / FindBoneNoCase(mesh, name)
 *     FindBoneChildren(mesh, boneIndex) / FindRootBones(mesh)
 *     BuildBoneOrder(mesh) / GetBoneDepth(mesh, boneIndex)
 *
 *   Validation & format info
 *     ValidateMesh(mesh, error)
 *     GetInfoPadding(version)
 *     GetAttributeTypeName(type) / GetAttributeUsageName(usage)
 *     GetAttributeTypeSize(type) / ComputeVertexStride(attributes)
 *
 * ============================================================================
 * DATA STRUCTURES
 * ============================================================================
 *   MeshData  - vertices, indices, materials, bones, bounds, and infoTail
 *               (hierarchy, collision, and setup data from the source MSH).
 *   Vertex    - position, normal, tangent, UV, material slot, four blend
 *               weights, and four bone indices.
 *   Bone      - name, 3x4 bind matrix, and parent index.
 *
 * ============================================================================
 * FORMAT NOTES
 * ============================================================================
 *   ContainerHeader (24 bytes) -> InfoBlock -> ModelBlock
 *   Strings use a u32 length prefix and are not null-terminated.
 *   Version 6 uses a 44-byte vertex stride:
 *     0x00 float3 position
 *     0x0C uint8  blendWeights[4]
 *     0x10 uint8  blendIndices[4]
 *     0x14 int8   packedNormal[4]
 *     0x18 float3 tangent
 *     0x24 uint8  color[4]
 *     0x28 int16  uv[2]
 *   Indices are uint16, limiting a model to 65535 vertices.
 *
 *   The writer creates version 6 files. If infoTail comes from a donor MSH,
 *   its hierarchy, collision, and physical setup data are preserved; only the
 *   bounds are updated. A new skeleton needs a compatible donor hierarchy to
 *   work with the game's existing animations.
 */

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace BullyMSH {

// ============================================================================
// DATA STRUCTURES
// ============================================================================

struct Vec2 {
    float x{};
    float y{};
    bool operator==(const Vec2& other) const { return x == other.x && y == other.y; }
};

struct Vec3 {
    float x{};
    float y{};
    float z{};
    bool operator==(const Vec3& other) const { return x == other.x && y == other.y && z == other.z; }
};

struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec3 tangent;
    Vec2 uv;
    uint32_t material{};
    uint8_t blendWeights[4]{255, 0, 0, 0};
    uint8_t blendIndices[4]{0, 0, 0, 0};
    uint8_t color[4]{0, 0, 0, 0};
};

struct Bone {
    std::string name;
    std::array<float, 12> matrix{
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0
    };
    int32_t parent = -1;
};

struct MeshData {
    std::string name = "editable poly";
    uint32_t version = 6;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<std::string> materials;
    std::vector<Bone> bones;
    std::vector<uint8_t> infoTail; // Preserved hierarchy, collision, and setup data from a donor MSH
    Vec3 low{};
    Vec3 high{};
};

struct BoneAdjustment {
    std::string name;
    Vec3 offset{};
    bool operator==(const BoneAdjustment& other) const { return name == other.name && offset == other.offset; }
};

struct ExportOptions {
    /** Model name stored in the MSH. If empty, uses MeshData::name. */
    std::string modelName;
    /** Material name to use when the mesh has no materials. */
    std::string materialName = "default";
    std::filesystem::path skinDonor;
    std::vector<BoneAdjustment> boneAdjustments;
    std::vector<BoneAdjustment> setupBoneAdjustments;
    float scale = 1.0f;
    bool flipV = true;
    bool keepAxes = false;
    bool reverseWinding = false;
};

struct SkinStats {
    float averageDistance{};
    float maximumDistance{};
};

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& value, float scale) { return {value.x * scale, value.y * scale, value.z * scale}; }
inline Vec3 operator*(float scale, const Vec3& value) { return value * scale; }
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline Vec3 Normalize(const Vec3& value) {
    const float length = std::sqrt(Dot(value, value));
    return length < 1.0e-8f ? Vec3{0, 0, 1} : value * (1.0f / length);
}

inline Vec3 ToBully(const Vec3& value, bool keepAxes) {
    return keepAxes ? value : Vec3{value.x, -value.z, value.y};
}

namespace Internal {

/** Check whether a string starts with the given character (C++17). */
inline bool StartsWith(const std::string& value, char prefix) {
    return !value.empty() && value.front() == prefix;
}

inline std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open file: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline void WriteFileBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("Cannot write file: " + path.string());
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

inline std::string LowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

inline std::vector<std::string> SplitString(const std::string& value) {
    std::istringstream input(value);
    std::vector<std::string> result;
    std::string part;
    while (input >> part) result.push_back(part);
    return result;
}

// Binary writing helpers
inline void WriteU8(std::vector<uint8_t>& out, uint8_t val) { out.push_back(val); }
inline void WriteU16(std::vector<uint8_t>& out, uint16_t val) {
    out.push_back(static_cast<uint8_t>(val));
    out.push_back(static_cast<uint8_t>(val >> 8));
}
inline void WriteU32(std::vector<uint8_t>& out, uint32_t val) {
    for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>(val >> s));
}
inline void WriteI32(std::vector<uint8_t>& out, int32_t val) { WriteU32(out, static_cast<uint32_t>(val)); }
inline void WriteFloat(std::vector<uint8_t>& out, float val) {
    uint32_t bits{};
    std::memcpy(&bits, &val, sizeof(bits));
    WriteU32(out, bits);
}
inline void WriteString(std::vector<uint8_t>& out, const std::string& str) {
    WriteU32(out, static_cast<uint32_t>(str.size()));
    out.insert(out.end(), str.begin(), str.end());
}
inline void PatchU32(std::vector<uint8_t>& out, size_t offset, uint32_t val) {
    for (int s = 0; s < 32; s += 8) out[offset + s / 8] = static_cast<uint8_t>(val >> s);
}

// Binary reader
class BinaryReader {
public:
    explicit BinaryReader(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
    uint8_t U8() { Require(1); return bytes_[offset_++]; }
    uint16_t U16() {
        Require(2);
        uint16_t v = static_cast<uint16_t>(bytes_[offset_]) | static_cast<uint16_t>(bytes_[offset_ + 1] << 8);
        offset_ += 2;
        return v;
    }
    uint32_t U32() {
        Require(4);
        uint32_t v{};
        for (int s = 0; s < 32; s += 8) v |= static_cast<uint32_t>(bytes_[offset_++]) << s;
        return v;
    }
    float Float() {
        uint32_t bits = U32();
        float f{};
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }
    std::string String() {
        uint32_t len = U32();
        Require(len);
        std::string s(reinterpret_cast<const char*>(bytes_.data() + offset_), len);
        offset_ += len;
        return s;
    }
    std::vector<uint8_t> Bytes(size_t n) {
        Require(n);
        std::vector<uint8_t> res(bytes_.begin() + offset_, bytes_.begin() + offset_ + n);
        offset_ += n;
        return res;
    }
    void Skip(size_t n) { Require(n); offset_ += n; }
    void Seek(size_t off) { if (off > bytes_.size()) throw std::runtime_error("Seek out of bounds"); offset_ = off; }
    size_t Tell() const { return offset_; }
    size_t Size() const { return bytes_.size(); }
private:
    void Require(size_t n) const { if (n > bytes_.size() - offset_) throw std::runtime_error("Unexpected EOF"); }
    std::vector<uint8_t> bytes_;
    size_t offset_{};
};

}

// ============================================================================
// LOADING OBJ AND MSH FILES
// ============================================================================

inline bool LoadObj(const std::filesystem::path& filepath, MeshData& mesh, std::string& err, float scale = 1.0f, bool flipV = true, bool keepAxes = false, bool reverseWinding = false) {
    try {
        std::ifstream input(filepath);
        if (!input) {
            err = "Cannot open OBJ file: " + filepath.string();
            return false;
        }

        struct ObjRef { int pos{}; int uv = -1; int norm = -1; };
        struct ObjTri { std::array<ObjRef, 3> corners; uint32_t mat{}; int smooth{}; uint32_t face{}; };

        std::vector<Vec3> positions;
        std::vector<Vec3> normals;
        std::vector<Vec2> uvs;
        std::vector<ObjTri> triangles;

        mesh = MeshData{};
        mesh.name = filepath.stem().string();
        uint32_t currentMat = 0;
        int smoothGroup = 0;
        uint32_t faceIdx = 0;
        std::string line;

        auto resolveIdx = [](int val, size_t count) {
            if (val > 0) return val - 1;
            if (val < 0) return static_cast<int>(count) + val;
            throw std::runtime_error("Invalid 0-based index in OBJ");
        };

        auto parseRef = [&](const std::string& token, size_t pSize, size_t uvSize, size_t nSize) {
            std::array<std::string, 3> fields;
            size_t start = 0;
            for (int i = 0; i < 3; ++i) {
                size_t slash = token.find('/', start);
                fields[i] = token.substr(start, slash == std::string::npos ? slash : slash - start);
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
            ObjRef ref;
            ref.pos = resolveIdx(std::stoi(fields[0]), pSize);
            if (!fields[1].empty()) ref.uv = resolveIdx(std::stoi(fields[1]), uvSize);
            if (!fields[2].empty()) ref.norm = resolveIdx(std::stoi(fields[2]), nSize);
            return ref;
        };

        while (std::getline(input, line)) {
            auto parts = Internal::SplitString(line);
            if (parts.empty() || Internal::StartsWith(parts[0], '#')) continue;
            if (parts[0] == "v" && parts.size() >= 4) {
                positions.push_back({std::stof(parts[1]) * scale, std::stof(parts[2]) * scale, std::stof(parts[3]) * scale});
            } else if (parts[0] == "vn" && parts.size() >= 4) {
                normals.push_back(Normalize({std::stof(parts[1]), std::stof(parts[2]), std::stof(parts[3])}));
            } else if (parts[0] == "vt" && parts.size() >= 3) {
                float v = std::stof(parts[2]);
                uvs.push_back({std::stof(parts[1]), flipV ? 1.0f - v : v});
            } else if (parts[0] == "usemtl" && parts.size() >= 2) {
                auto found = std::find(mesh.materials.begin(), mesh.materials.end(), parts[1]);
                if (found == mesh.materials.end()) {
                    mesh.materials.push_back(parts[1]);
                    currentMat = static_cast<uint32_t>(mesh.materials.size() - 1);
                } else {
                    currentMat = static_cast<uint32_t>(std::distance(mesh.materials.begin(), found));
                }
            } else if (parts[0] == "s" && parts.size() >= 2) {
                if (parts[1] == "off" || parts[1] == "0") smoothGroup = 0;
                else if (parts[1] == "on") smoothGroup = 1;
                else smoothGroup = std::max(1, std::stoi(parts[1]));
            } else if (parts[0] == "f" && parts.size() >= 4) {
                if (mesh.materials.empty()) {
                    mesh.materials.push_back("default");
                    currentMat = 0;
                }
                std::vector<ObjRef> polygon;
                for (size_t i = 1; i < parts.size() && !Internal::StartsWith(parts[i], '#'); ++i) {
                    polygon.push_back(parseRef(parts[i], positions.size(), uvs.size(), normals.size()));
                }
                if (polygon.size() < 3) continue;
                for (size_t i = 1; i + 1 < polygon.size(); ++i) {
                    ObjTri tri{{polygon[0], polygon[i], polygon[i + 1]}, currentMat, smoothGroup, faceIdx++};
                    if (reverseWinding) std::swap(tri.corners[1], tri.corners[2]);
                    triangles.push_back(tri);
                }
            }
        }

        if (triangles.empty()) {
            err = "OBJ contains no triangles";
            return false;
        }

        // Build unique vertices and accumulate normals and tangents.
        struct VertexKey {
            int pos{}; int uv{}; int norm{}; int smooth{}; uint32_t mat{};
            bool operator==(const VertexKey& o) const { return pos == o.pos && uv == o.uv && norm == o.norm && smooth == o.smooth && mat == o.mat; }
        };
        struct Hash {
            size_t operator()(const VertexKey& k) const {
                size_t v = static_cast<size_t>(k.pos + 1);
                v = v * 16777619u ^ static_cast<size_t>(k.uv + 2);
                v = v * 16777619u ^ static_cast<size_t>(k.norm + 2);
                v = v * 16777619u ^ static_cast<size_t>(k.smooth);
                v = v * 16777619u ^ static_cast<size_t>(k.mat);
                return v;
            }
        };
        struct NormalKey { int pos{}; int smooth{}; bool operator==(const NormalKey& o) const { return pos == o.pos && smooth == o.smooth; } };
        struct NormalHash {
            size_t operator()(const NormalKey& k) const {
                return static_cast<size_t>(k.pos + 1) * 16777619u ^ static_cast<size_t>(k.smooth);
            }
        };
        struct BuildData {
            Vec3 tangentSum{}; Vec3 faceNormalSum{};
            bool explicitNormal{}; int sourcePosition{}; int smooth{};
        };

        auto fallbackTangent = [](const Vec3& n) {
            const Vec3 axis = std::abs(n.z) < 0.999f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
            return Normalize(Cross(axis, n));
        };

        std::unordered_map<VertexKey, uint32_t, Hash> vMap;
        std::unordered_map<NormalKey, Vec3, NormalHash> normalSums;
        std::vector<BuildData> buildData;

        for (const auto& tri : triangles) {
            std::array<uint32_t, 3> triIdx;
            for (int c = 0; c < 3; ++c) {
                const auto& ref = tri.corners[c];
                // Use explicit normals first, then smoothing groups. With `s off`,
                // each face corner gets its own key for flat shading.
                const int smoothKey = ref.norm >= 0 ? ref.norm + 1 :
                                      tri.smooth > 0 ? tri.smooth :
                                      -static_cast<int>(tri.face * 3 + c + 1);
                VertexKey key{ref.pos, ref.uv, ref.norm, smoothKey, tri.mat};
                auto it = vMap.find(key);
                if (it != vMap.end()) {
                    triIdx[c] = it->second;
                    continue;
                }
                Vertex vert;
                vert.position = ToBully(positions[ref.pos], keepAxes);
                if (ref.norm >= 0) vert.normal = Normalize(ToBully(normals[ref.norm], keepAxes));
                if (ref.uv >= 0) vert.uv = uvs[ref.uv];
                vert.material = tri.mat;

                uint32_t newIdx = static_cast<uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(vert);
                buildData.push_back({{}, {}, ref.norm >= 0, ref.pos, smoothKey});
                vMap.emplace(key, newIdx);
                triIdx[c] = newIdx;
            }

            const Vec3 edge1 = mesh.vertices[triIdx[1]].position - mesh.vertices[triIdx[0]].position;
            const Vec3 edge2 = mesh.vertices[triIdx[2]].position - mesh.vertices[triIdx[0]].position;
            const Vec3 faceNormal = Cross(edge1, edge2);

            const Vec2 uv0 = mesh.vertices[triIdx[0]].uv;
            const Vec2 uv1 = mesh.vertices[triIdx[1]].uv;
            const Vec2 uv2 = mesh.vertices[triIdx[2]].uv;
            const float du1 = uv1.x - uv0.x, dv1 = uv1.y - uv0.y;
            const float du2 = uv2.x - uv0.x, dv2 = uv2.y - uv0.y;
            const float det = du1 * dv2 - du2 * dv1;
            Vec3 tangent = edge1;
            if (std::abs(det) > 1.0e-8f) {
                const float inv = 1.0f / det;
                tangent = {inv * (dv2 * edge1.x - dv1 * edge2.x),
                           inv * (dv2 * edge1.y - dv1 * edge2.y),
                           inv * (dv2 * edge1.z - dv1 * edge2.z)};
            }

            for (int c = 0; c < 3; ++c) {
                const uint32_t index = triIdx[c];
                buildData[index].faceNormalSum = buildData[index].faceNormalSum + faceNormal;
                if (!buildData[index].explicitNormal) {
                    const NormalKey key{buildData[index].sourcePosition, buildData[index].smooth};
                    normalSums[key] = normalSums[key] + faceNormal;
                }
                buildData[index].tangentSum = buildData[index].tangentSum + tangent;
            }

            mesh.indices.push_back(triIdx[0]);
            mesh.indices.push_back(triIdx[1]);
            mesh.indices.push_back(triIdx[2]);
        }

        // Finish generated normals and make tangents perpendicular to them.
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            Vertex& vert = mesh.vertices[i];
            const BuildData& build = buildData[i];
            if (!build.explicitNormal) {
                auto found = normalSums.find({build.sourcePosition, build.smooth});
                vert.normal = found != normalSums.end() ? Normalize(found->second) : Vec3{0, 0, 1};
            } else {
                vert.normal = Normalize(vert.normal);
            }
            Vec3 tangent = build.tangentSum - vert.normal * Dot(vert.normal, build.tangentSum);
            vert.tangent = Dot(tangent, tangent) > 1.0e-8f ? Normalize(tangent) : fallbackTangent(vert.normal);
        }

        // Flip corner normals that point against their face.
        const size_t originalCount = mesh.vertices.size();
        std::vector<uint32_t> flipped(originalCount, std::numeric_limits<uint32_t>::max());
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const Vertex& a = mesh.vertices[mesh.indices[t]];
            const Vertex& b = mesh.vertices[mesh.indices[t + 1]];
            const Vertex& c = mesh.vertices[mesh.indices[t + 2]];
            const Vec3 faceNormal = Normalize(Cross(b.position - a.position, c.position - a.position));
            for (int corner = 0; corner < 3; ++corner) {
                uint32_t& vertexIndex = mesh.indices[t + corner];
                const Vertex& vert = mesh.vertices[vertexIndex];
                if (Dot(vert.normal, faceNormal) >= 0.0f) continue;
                if (vertexIndex < originalCount && flipped[vertexIndex] != std::numeric_limits<uint32_t>::max()) {
                    vertexIndex = flipped[vertexIndex];
                    continue;
                }
                Vertex repaired = vert;
                repaired.normal = repaired.normal * -1.0f;
                const uint32_t repairedIndex = static_cast<uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(repaired);
                if (vertexIndex < originalCount) flipped[vertexIndex] = repairedIndex;
                vertexIndex = repairedIndex;
            }
        }
        return true;
    } catch (const std::exception& e) {
        err = std::string("LoadObj error: ") + e.what();
        return false;
    }
}

inline bool LoadMsh(const std::filesystem::path& filepath, MeshData& mesh, std::string& err) {
    try {
        std::vector<uint8_t> fileBytes = Internal::ReadFileBytes(filepath);
        Internal::BinaryReader reader(fileBytes);

        uint32_t version = reader.U32();
        uint32_t blockCount = reader.U32();
        if (blockCount < 2) {
            err = "Invalid MSH container block count";
            return false;
        }
        reader.U32(); // tag
        uint32_t infoOffset = reader.U32();
        reader.U32(); // model count
        uint32_t modelOffset = reader.U32();

        reader.Seek(infoOffset);
        mesh = MeshData{};
        mesh.version = version;

        uint32_t matCount = reader.U32();
        for (uint32_t i = 0; i < matCount; ++i) mesh.materials.push_back(reader.String());

        uint32_t boneCount = reader.U32();
        mesh.bones.reserve(boneCount);
        for (uint32_t i = 0; i < boneCount; ++i) {
            Bone b;
            b.name = reader.String();
            for (float& val : b.matrix) val = reader.Float();
            b.parent = static_cast<int32_t>(reader.U32());
            mesh.bones.push_back(b);
        }

        size_t infoTailStart = reader.Tell();
        size_t padding = (version == 6) ? 1 : (version == 7) ? 2 : (version == 8) ? 3 : (version >= 12) ? 5 : 4;
        reader.Skip(padding);
        mesh.low = {reader.Float(), reader.Float(), reader.Float()};
        mesh.high = {reader.Float(), reader.Float(), reader.Float()};
        reader.Skip(16); // center & radius

        mesh.infoTail.assign(fileBytes.begin() + infoTailStart, fileBytes.begin() + modelOffset);

        reader.Seek(modelOffset);
        reader.U32(); // model count
        mesh.name = reader.String();
        reader.Skip(1); // flag0
        if (version > 4) reader.Skip(1); // flag1
        if (version > 9) reader.Skip(version == 10 ? 12 : 16);

        uint32_t vertexCount = reader.U32();
        uint32_t indexCount = reader.U32();
        mesh.indices.resize(indexCount);
        for (uint32_t i = 0; i < indexCount; ++i) mesh.indices[i] = reader.U16();

        uint32_t attrCount = reader.U32();
        size_t stride = 0;
        struct AttrDecl { uint32_t type; uint32_t semantic; size_t offset; size_t size; };
        std::vector<AttrDecl> attrs;
        for (uint32_t i = 0; i < attrCount; ++i) {
            uint32_t type = reader.U32();
            uint32_t sem = reader.U32();
            reader.Skip(2);
            size_t sz = (type == 2 || type == 3) ? 12 : (type == 8 || type == 9 || type == 10 || type == 11 || type == 12) ? 4 : 8;
            attrs.push_back({type, sem, stride, sz});
            stride += sz;
        }

        auto payload = reader.Bytes(vertexCount * stride);
        mesh.vertices.resize(vertexCount);
        for (uint32_t v = 0; v < vertexCount; ++v) {
            Vertex& vert = mesh.vertices[v];
            const uint8_t* base = payload.data() + v * stride;
            for (const auto& attr : attrs) {
                const uint8_t* ptr = base + attr.offset;
                if (attr.semantic == 0 && attr.size == 12) std::memcpy(&vert.position, ptr, 12);
                else if (attr.semantic == 1 && attr.size == 4) std::memcpy(vert.blendWeights, ptr, 4);
                else if (attr.semantic == 2 && attr.size == 4) std::memcpy(vert.blendIndices, ptr, 4);
                else if (attr.semantic == 3 && attr.size == 4) {
                    const int8_t* p = reinterpret_cast<const int8_t*>(ptr);
                    vert.normal = Normalize({p[0] / 127.0f, p[1] / 127.0f, p[2] / 127.0f});
                }                 else if (attr.semantic == 5 && attr.size == 12) std::memcpy(&vert.tangent, ptr, 12);
                else if (attr.semantic == 5 && attr.size == 4) {
                    const int8_t* p = reinterpret_cast<const int8_t*>(ptr);
                    vert.tangent = Normalize({p[0] / 127.0f, p[1] / 127.0f, p[2] / 127.0f});
                }
                else if (attr.semantic == 6 && attr.size == 4) std::memcpy(vert.color, ptr, 4);
                else if (attr.semantic == 4 && attr.size == 4) {
                    int16_t pk[2];
                    std::memcpy(pk, ptr, 4);
                    vert.uv = {pk[0] / 2048.0f, pk[1] / 2048.0f};
                }
            }
        }

        // Assign each vertex its material slot from the MSH groups.
        const uint32_t groupCount = reader.U32();
        if (groupCount > 0) {
            const std::vector<uint32_t> sourceIndices = mesh.indices;
            const std::vector<Vertex> sourceVertices = mesh.vertices;
            mesh.indices.clear();
            mesh.vertices.clear();

            std::unordered_map<uint64_t, uint32_t> remap;
            for (uint32_t g = 0; g < groupCount; ++g) {
                const uint32_t firstIndex = reader.U32();
                const uint32_t numIndices = reader.U32();
                const uint32_t materialIndex = reader.U32();
                const uint32_t primitiveType = reader.U32();
                if (primitiveType != 0) {
                    err = "MSH triangle strips are not supported";
                    return false;
                }
                if (firstIndex > sourceIndices.size() ||
                    numIndices > sourceIndices.size() - firstIndex) {
                    err = "MSH material group is outside index data";
                    return false;
                }
                for (uint32_t o = 0; o < numIndices; ++o) {
                    const uint32_t sourceIndex = sourceIndices[firstIndex + o];
                    if (sourceIndex >= sourceVertices.size()) {
                        err = "MSH index is outside vertex data";
                        return false;
                    }
                    const uint64_t key = (static_cast<uint64_t>(materialIndex) << 32) | sourceIndex;
                    auto found = remap.find(key);
                    if (found != remap.end()) {
                        mesh.indices.push_back(found->second);
                        continue;
                    }
                    Vertex vert = sourceVertices[sourceIndex];
                    vert.material = materialIndex;
                    const uint32_t destination = static_cast<uint32_t>(mesh.vertices.size());
                    mesh.vertices.push_back(vert);
                    mesh.indices.push_back(destination);
                    remap.emplace(key, destination);
                }
            }
        }
        return true;
    } catch (const std::exception& e) {
        err = std::string("LoadMsh error: ") + e.what();
        return false;
    }
}

// ============================================================================
// WRITING OBJ AND MSH FILES
// ============================================================================

inline bool SaveObj(const std::filesystem::path& filepath, const MeshData& mesh, std::string& err) {
    try {
        if (!filepath.parent_path().empty()) std::filesystem::create_directories(filepath.parent_path());
        std::ofstream file(filepath);
        if (!file) {
            err = "Cannot write OBJ file: " + filepath.string();
            return false;
        }
        file << std::fixed << std::setprecision(7);
        file << "# BullyMSH Exporter\n";
        file << "o " << mesh.name << "\n";
        for (const auto& v : mesh.vertices) file << "v " << v.position.x << ' ' << v.position.z << ' ' << -v.position.y << "\n";
        for (const auto& v : mesh.vertices) file << "vt " << v.uv.x << ' ' << 1.0f - v.uv.y << "\n";
        for (const auto& v : mesh.vertices) file << "vn " << v.normal.x << ' ' << v.normal.z << ' ' << -v.normal.y << "\n";

        uint32_t activeMat = UINT32_MAX;
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            uint32_t idx0 = mesh.indices[i];
            if (idx0 >= mesh.vertices.size()) continue;
            uint32_t mat = mesh.vertices[idx0].material;
            if (mat != activeMat) {
                activeMat = mat;
                std::string mName = (mat < mesh.materials.size()) ? mesh.materials[mat] : "default";
                file << "usemtl " << mName << "\n";
            }
            file << "f";
            for (int c = 0; c < 3; ++c) {
                uint32_t oIdx = mesh.indices[i + c] + 1;
                file << ' ' << oIdx << '/' << oIdx << '/' << oIdx;
            }
            file << "\n";
        }
        return true;
    } catch (const std::exception& e) {
        err = std::string("SaveObj error: ") + e.what();
        return false;
    }
}

/** Number of padding bytes after the bone table for this version. */
inline uint32_t GetInfoPadding(uint32_t version);

/**
 * Writes MeshData to a little-endian Bully MSH file.
 *
 * Output uses version 6, a 44-byte vertex layout (position, blend weights,
 * blend indices, packed normal, tangent, color, and UV), and material groups.
 *
 * If infoTail was loaded from a donor MSH, its hierarchy, collision, and
 * physical setup data are preserved. The bounds are updated for the new mesh.
 */
inline bool SaveMsh(const std::filesystem::path& filepath, const MeshData& mesh, const ExportOptions& options, std::string& err) {
    try {
        if (mesh.vertices.empty() || mesh.indices.empty()) {
            err = "Mesh contains no vertices or triangles";
            return false;
        }
        for (uint32_t index : mesh.indices) {
            if (index >= mesh.vertices.size()) {
                err = "Mesh index is outside the vertex range";
                return false;
            }
        }

        // Group indices by material slot.
        std::map<uint32_t, std::vector<uint32_t>> byMaterial;
        for (uint32_t index : mesh.indices) byMaterial[mesh.vertices[index].material].push_back(index);

        // Pack vertices and remove duplicates (16-bit index limit).
        auto packSnorm = [](float val) { return static_cast<int8_t>(std::clamp(std::lround(val * 127.0f), -127l, 127l)); };
        auto packUv = [](float val) { return static_cast<int16_t>(std::clamp(std::lround(val * 2048.0f), -32768l, 32767l)); };

        auto packVertex = [&](const Vertex& v) {
            std::vector<uint8_t> bytes;
            bytes.reserve(44);
            Internal::WriteFloat(bytes, v.position.x);
            Internal::WriteFloat(bytes, v.position.y);
            Internal::WriteFloat(bytes, v.position.z);
            bytes.insert(bytes.end(), std::begin(v.blendWeights), std::end(v.blendWeights));
            bytes.insert(bytes.end(), std::begin(v.blendIndices), std::end(v.blendIndices));
            Internal::WriteU8(bytes, static_cast<uint8_t>(packSnorm(v.normal.x)));
            Internal::WriteU8(bytes, static_cast<uint8_t>(packSnorm(v.normal.y)));
            Internal::WriteU8(bytes, static_cast<uint8_t>(packSnorm(v.normal.z)));
            Internal::WriteU8(bytes, 0);
            Internal::WriteFloat(bytes, v.tangent.x);
            Internal::WriteFloat(bytes, v.tangent.y);
            Internal::WriteFloat(bytes, v.tangent.z);
            bytes.insert(bytes.end(), std::begin(v.color), std::end(v.color));
            Internal::WriteU16(bytes, static_cast<uint16_t>(packUv(v.uv.x)));
            Internal::WriteU16(bytes, static_cast<uint16_t>(packUv(v.uv.y)));
            return bytes;
        };

        std::vector<uint8_t> packedVertices;
        std::vector<uint16_t> packedIndices;
        struct Group { uint32_t first{}; uint32_t count{}; uint32_t material{}; };
        std::vector<Group> groups;

        Vec3 low{1e9f, 1e9f, 1e9f};
        Vec3 high{-1e9f, -1e9f, -1e9f};
        std::unordered_map<std::string, uint16_t> unique;
        std::map<uint32_t, Vec3> groupLow;
        std::map<uint32_t, Vec3> groupHigh;

        for (const auto& entry : byMaterial) {
            const uint32_t material = entry.first;
            Group group;
            group.first = static_cast<uint32_t>(packedIndices.size());
            group.material = material;

            Vec3 gLow{1e9f, 1e9f, 1e9f};
            Vec3 gHigh{-1e9f, -1e9f, -1e9f};

            for (uint32_t sourceIndex : entry.second) {
                const Vertex& vertex = mesh.vertices[sourceIndex];
                const std::vector<uint8_t> packed = packVertex(vertex);
                const std::string key(reinterpret_cast<const char*>(packed.data()), packed.size());

                uint16_t destination{};
                auto found = unique.find(key);
                if (found != unique.end()) {
                    destination = found->second;
                } else {
                    if (unique.size() >= 65535) {
                        err = "Mesh exceeds the 65535 vertex limit of the MSH index format";
                        return false;
                    }
                    destination = static_cast<uint16_t>(unique.size());
                    unique.emplace(key, destination);
                    packedVertices.insert(packedVertices.end(), packed.begin(), packed.end());
                    low.x = std::min(low.x, vertex.position.x);
                    low.y = std::min(low.y, vertex.position.y);
                    low.z = std::min(low.z, vertex.position.z);
                    high.x = std::max(high.x, vertex.position.x);
                    high.y = std::max(high.y, vertex.position.y);
                    high.z = std::max(high.z, vertex.position.z);
                    gLow.x = std::min(gLow.x, vertex.position.x);
                    gLow.y = std::min(gLow.y, vertex.position.y);
                    gLow.z = std::min(gLow.z, vertex.position.z);
                    gHigh.x = std::max(gHigh.x, vertex.position.x);
                    gHigh.y = std::max(gHigh.y, vertex.position.y);
                    gHigh.z = std::max(gHigh.z, vertex.position.z);
                }
                packedIndices.push_back(destination);
            }

            group.count = static_cast<uint32_t>(packedIndices.size()) - group.first;
            groups.push_back(group);
            groupLow[material] = gLow;
            groupHigh[material] = gHigh;
        }

        const Vec3 center = (low + high) * 0.5f;
        const Vec3 extent = high - center;
        const float radius = std::sqrt(Dot(extent, extent));
        const std::array<float, 10> bounds{
            low.x, low.y, low.z, high.x, high.y, high.z, center.x, center.y, center.z, radius
        };

        // Write the container header.
        std::vector<uint8_t> out;
        Internal::WriteU32(out, 6);      // format version
        Internal::WriteU32(out, 2);      // block count
        Internal::WriteU32(out, 0x539);  // resource tag
        Internal::WriteU32(out, 0x18);   // info offset
        Internal::WriteU32(out, 1);      // model count
        const size_t modelOffsetPos = out.size();
        Internal::WriteU32(out, 0);      // model offset placeholder

        // Write the material list.
        std::vector<std::string> materials = mesh.materials;
        if (materials.empty()) materials.push_back(options.materialName);
        Internal::WriteU32(out, static_cast<uint32_t>(materials.size()));
        for (const std::string& material : materials) Internal::WriteString(out, material);

        // Write the bone list.
        std::vector<Bone> bones = mesh.bones;
        if (bones.empty()) bones.push_back(Bone{"root"});
        Internal::WriteU32(out, static_cast<uint32_t>(bones.size()));
        for (const Bone& bone : bones) {
            Internal::WriteString(out, bone.name);
            for (float value : bone.matrix) Internal::WriteFloat(out, value);
            Internal::WriteI32(out, bone.parent);
        }

        // Write padding, bounds, and the remaining info data.
        const size_t padding = GetInfoPadding(6);
        if (!mesh.infoTail.empty()) {
            // Keep the donor hierarchy, collision, and setup data; update only
            // the ten bounds values.
            if (mesh.infoTail.size() < padding + 40) {
                err = "Donor info block is truncated";
                return false;
            }
            std::vector<uint8_t> tail{0};
            tail.insert(tail.end(),
                        mesh.infoTail.begin() + static_cast<std::ptrdiff_t>(padding),
                        mesh.infoTail.end());
            if (tail.size() < 1 + bounds.size() * sizeof(float)) {
                err = "Donor info block is too small to hold the bounds";
                return false;
            }
            for (size_t i = 0; i < bounds.size(); ++i) {
                std::memcpy(tail.data() + 1 + i * sizeof(float), &bounds[i], sizeof(float));
            }
            out.insert(out.end(), tail.begin(), tail.end());
        } else {
            // Default tail used when no donor MSH is provided.
            Internal::WriteU8(out, 0);
            for (float value : bounds) Internal::WriteFloat(out, value);
            Internal::WriteString(out, "{physical(physicalmaterial)=,setups(orderedarray<collisionshapesetup>)=[0]}");
            Internal::WriteString(out, "[0]");
            Internal::WriteU32(out, 1);
            Internal::WriteString(out, "Root");
            Internal::WriteU32(out, 0); Internal::WriteU32(out, 0);
            Internal::WriteU32(out, 0); Internal::WriteU32(out, 1);
            Internal::WriteU32(out, 1);
            Internal::WriteString(out, "Root");
            Internal::WriteU32(out, 0); Internal::WriteU32(out, 0xffffffff);
            Internal::WriteU32(out, 1); Internal::WriteU32(out, 0xffffffff);
            for (float value : std::array<float, 12>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0})
                Internal::WriteFloat(out, value);
        }

        Internal::PatchU32(out, modelOffsetPos, static_cast<uint32_t>(out.size()));

        // Write the model block.
        Internal::WriteU32(out, 1); // model count
        Internal::WriteString(out, options.modelName.empty() ? mesh.name : options.modelName);
        Internal::WriteU8(out, 0); Internal::WriteU8(out, 0);

        Internal::WriteU32(out, static_cast<uint32_t>(packedVertices.size() / 44));
        Internal::WriteU32(out, static_cast<uint32_t>(packedIndices.size()));
        for (uint16_t index : packedIndices) Internal::WriteU16(out, index);

        // Declare the 7 vertex attributes (44 bytes per vertex).
        Internal::WriteU32(out, 7);
        auto writeAttr = [&](uint32_t type, uint32_t semantic) {
            Internal::WriteU32(out, type);
            Internal::WriteU32(out, semantic);
            Internal::WriteU8(out, 0);
            Internal::WriteU8(out, 1);
        };
        writeAttr(2, 0);   // position   float3
        writeAttr(9, 1);   // weights    normalized u8x4
        writeAttr(8, 2);   // bone index u8x4
        writeAttr(10, 3);  // normal     snorm packed
        writeAttr(2, 5);   // tangent    float3
        writeAttr(8, 6);   // color      u8x4
        writeAttr(11, 4);  // uv         i16x2

        out.insert(out.end(), packedVertices.begin(), packedVertices.end());

        // Write material groups.
        Internal::WriteU32(out, static_cast<uint32_t>(groups.size()));
        for (const Group& group : groups) {
            Internal::WriteU32(out, group.first);
            Internal::WriteU32(out, group.count);
            Internal::WriteU32(out, group.material);
            Internal::WriteU32(out, 0); // primitive type: triangle list
        }

        Internal::WriteFileBytes(filepath, out);
        return true;
    } catch (const std::exception& e) {
        err = std::string("SaveMsh error: ") + e.what();
        return false;
    }
}

// ============================================================================
// SKINNING AND DEFORMATION
// ============================================================================

inline SkinStats TransferSkin(MeshData& destination, const MeshData& donor) {
    if (donor.bones.size() < 2 || donor.infoTail.empty())
        throw std::runtime_error("Skin donor does not contain a usable skinned hierarchy");
    if (donor.vertices.empty()) throw std::runtime_error("Skin donor contains no vertices");

    SkinStats stats;
    double distSum = 0.0;
    for (auto& v : destination.vertices) {
        const Vertex* nearest = nullptr;
        float minDistSq = std::numeric_limits<float>::max();
        for (const auto& dVert : donor.vertices) {
            Vec3 delta = v.position - dVert.position;
            float distSq = Dot(delta, delta);
            if (distSq < minDistSq) {
                minDistSq = distSq;
                nearest = &dVert;
            }
        }
        std::copy(std::begin(nearest->blendWeights), std::end(nearest->blendWeights), std::begin(v.blendWeights));
        std::copy(std::begin(nearest->blendIndices), std::end(nearest->blendIndices), std::begin(v.blendIndices));
        float dist = std::sqrt(minDistSq);
        distSum += dist;
        stats.maximumDistance = std::max(stats.maximumDistance, dist);
    }
    stats.averageDistance = destination.vertices.empty() ? 0.0f : static_cast<float>(distSum / destination.vertices.size());
    destination.bones = donor.bones;
    destination.infoTail = donor.infoTail;
    destination.version = donor.version;
    return stats;
}

// ============================================================================
// BONE HIERARCHY UTILITIES
// ============================================================================

/** Finds a bone by exact name; returns -1 if it is not found. */
inline int FindBone(const MeshData& mesh, const std::string& name) {
    for (size_t i = 0; i < mesh.bones.size(); ++i) {
        if (mesh.bones[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

/** Finds a bone by name, ignoring case; returns -1 if not found. */
inline int FindBoneNoCase(const MeshData& mesh, const std::string& name) {
    for (size_t i = 0; i < mesh.bones.size(); ++i) {
        if (mesh.bones[i].name.size() != name.size()) continue;
        bool match = true;
        for (size_t c = 0; c < name.size(); ++c) {
            if (std::tolower(static_cast<unsigned char>(mesh.bones[i].name[c])) !=
                std::tolower(static_cast<unsigned char>(name[c]))) {
                match = false;
                break;
            }
        }
        if (match) return static_cast<int>(i);
    }
    return -1;
}

/** Returns the direct children of a bone. */
inline std::vector<int> FindBoneChildren(const MeshData& mesh, int boneIndex) {
    std::vector<int> children;
    for (size_t i = 0; i < mesh.bones.size(); ++i) {
        if (mesh.bones[i].parent == boneIndex) children.push_back(static_cast<int>(i));
    }
    return children;
}

/** Returns bones whose parent index is -1. */
inline std::vector<int> FindRootBones(const MeshData& mesh) {
    std::vector<int> roots;
    for (size_t i = 0; i < mesh.bones.size(); ++i) {
        if (mesh.bones[i].parent < 0) roots.push_back(static_cast<int>(i));
    }
    return roots;
}

/** Returns bones in depth-first order from the roots; skips cycles. */
inline std::vector<int> BuildBoneOrder(const MeshData& mesh) {
    std::vector<int> order;
    std::vector<uint8_t> visited(mesh.bones.size(), 0);
    const auto walk = [&](const auto& self, int index) -> void {
        if (index < 0 || static_cast<size_t>(index) >= mesh.bones.size()) return;
        if (visited[index]) return;
        visited[index] = 1;
        order.push_back(index);
        for (int child : FindBoneChildren(mesh, index)) self(self, child);
    };
    for (int root : FindRootBones(mesh)) walk(walk, root);
    // Also include bones that are not reachable from a root.
    for (size_t i = 0; i < mesh.bones.size(); ++i) {
        if (!visited[i]) order.push_back(static_cast<int>(i));
    }
    return order;
}

/** Returns the number of ancestors above a bone. */
inline int GetBoneDepth(const MeshData& mesh, int boneIndex) {
    int depth = 0;
    int current = boneIndex;
    while (current >= 0 && static_cast<size_t>(current) < mesh.bones.size()) {
        const int32_t parent = mesh.bones[current].parent;
        if (parent < 0 || parent == current) break;
        current = parent;
        ++depth;
        if (depth > static_cast<int>(mesh.bones.size())) break; // stop if the hierarchy contains a cycle
    }
    return depth;
}

// ============================================================================
// SKIN WEIGHT UTILITIES
// ============================================================================

/** Returns the sum of a vertex’s four blend weights (0–255 scale). */
inline int GetTotalBlendWeight(const Vertex& vertex) {
    return static_cast<int>(vertex.blendWeights[0]) +
           static_cast<int>(vertex.blendWeights[1]) +
           static_cast<int>(vertex.blendWeights[2]) +
           static_cast<int>(vertex.blendWeights[3]);
}

/**
 * Rescales a vertex's blend weights so they sum to 255.
 * Empty weight slots are moved to the end.
 */
inline void NormalizeBlendWeights(Vertex& vertex) {
    int total = GetTotalBlendWeight(vertex);
    if (total <= 0) {
        vertex.blendWeights[0] = 255;
        vertex.blendWeights[1] = 0;
        vertex.blendWeights[2] = 0;
        vertex.blendWeights[3] = 0;
        return;
    }
    int accumulated = 0;
    for (int i = 0; i < 3; ++i) {
        const int value = static_cast<int>(std::lround(
            static_cast<double>(vertex.blendWeights[i]) * 255.0 / total));
        vertex.blendWeights[i] = static_cast<uint8_t>(std::clamp(value, 0, 255));
        accumulated += vertex.blendWeights[i];
    }
    vertex.blendWeights[3] = static_cast<uint8_t>(std::clamp(255 - accumulated, 0, 255));
}

/** Sets a vertex's four bone influences and normalizes the weights. */
inline void SetVertexSkin(Vertex& vertex, const uint8_t boneIndices[4], const uint8_t weights[4]) {
    for (int i = 0; i < 4; ++i) {
        vertex.blendIndices[i] = boneIndices[i];
        vertex.blendWeights[i] = weights[i];
    }
    NormalizeBlendWeights(vertex);
}

/** Counts vertices influenced by the specified bone index. */
inline size_t CountVerticesUsingBone(const MeshData& mesh, int boneIndex) {
    size_t count = 0;
    for (const Vertex& vertex : mesh.vertices) {
        for (int slot = 0; slot < 4; ++slot) {
            if (vertex.blendWeights[slot] > 0 && vertex.blendIndices[slot] == boneIndex) {
                ++count;
                break;
            }
        }
    }
    return count;
}

/** Returns the number of vertices influenced by each bone index. */
inline std::vector<size_t> BuildBoneUsageTable(const MeshData& mesh) {
    std::vector<size_t> usage(mesh.bones.size(), 0);
    for (const Vertex& vertex : mesh.vertices) {
        for (int slot = 0; slot < 4; ++slot) {
            if (vertex.blendWeights[slot] == 0) continue;
            const size_t bone = vertex.blendIndices[slot];
            if (bone < usage.size()) ++usage[bone];
        }
    }
    return usage;
}

// ============================================================================
// MESH VALIDATION
// ============================================================================

/**
 * Checks a mesh before saving or after loading.
 * Validates geometry, material references, index limits, bone parents, and
 * skin-weight bone indices.
 */
inline bool ValidateMesh(const MeshData& mesh, std::string& err) {
    if (mesh.vertices.empty()) {
        err = "Mesh has no vertices";
        return false;
    }
    if (mesh.indices.empty() || mesh.indices.size() % 3 != 0) {
        err = "Mesh index count is not a multiple of 3";
        return false;
    }
    if (mesh.indices.size() / 3 > 65535) {
        err = "Mesh exceeds the 65535 triangle limit of the MSH index format";
        return false;
    }
    for (size_t i = 0; i < mesh.indices.size(); ++i) {
        if (mesh.indices[i] >= mesh.vertices.size()) {
            err = "Index " + std::to_string(i) + " is out of vertex range";
            return false;
        }
    }
    if (mesh.vertices.size() > 65535) {
        err = "Mesh exceeds the 65535 vertex limit of the MSH index format";
        return false;
    }
    if (mesh.materials.empty()) {
        err = "Mesh has no material slots";
        return false;
    }
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (mesh.vertices[i].material >= mesh.materials.size()) {
            err = "Vertex " + std::to_string(i) + " references a missing material slot";
            return false;
        }
    }
    for (size_t i = 0; i < mesh.bones.size(); ++i) {
        const int32_t parent = mesh.bones[i].parent;
        if (parent >= static_cast<int32_t>(mesh.bones.size())) {
            err = "Bone '" + mesh.bones[i].name + "' has an out-of-range parent index";
            return false;
        }
        if (parent == static_cast<int32_t>(i)) {
            err = "Bone '" + mesh.bones[i].name + "' is its own parent";
            return false;
        }
    }
    // Check bone indices only when the mesh has a skeleton. Static meshes can
    // have no bones.
    if (!mesh.bones.empty()) {
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            const Vertex& vertex = mesh.vertices[i];
            for (int slot = 0; slot < 4; ++slot) {
                if (vertex.blendWeights[slot] > 0 &&
                    vertex.blendIndices[slot] >= mesh.bones.size()) {
                    err = "Vertex " + std::to_string(i) + " references bone index " +
                          std::to_string(vertex.blendIndices[slot]) + " but the mesh has only " +
                          std::to_string(mesh.bones.size()) + " bones";
                    return false;
                }
            }
        }
    }
    return true;
}

// ============================================================================
// MSH FORMAT INFORMATION
// ============================================================================

/** Number of padding bytes after the bone table for this version. */
inline uint32_t GetInfoPadding(uint32_t version) {
    return version == 6 ? 1 : version == 7 ? 2 :
           version == 8 ? 3 : version >= 12 ? 5 : 4;
}

/** Returns the readable name of a vertex attribute type. */
inline const char* GetAttributeTypeName(uint32_t type) {
    switch (type) {
        case 1: return "unknown-2-comp";
        case 2: return "float3";
        case 3: return "unknown-3-comp";
        case 4: return "unknown-2-comp";
        case 8: return "u8x4";
        case 9: return "normalized-u8x4";
        case 10: return "snorm-packed";
        case 11: return "i16x2";
        case 12: return "unknown-packed";
        default: return "invalid";
    }
}

/** Returns the readable name of a vertex attribute usage. */
inline const char* GetAttributeUsageName(uint32_t usage) {
    switch (usage) {
        case 0: return "position";
        case 1: return "blend-weights";
        case 2: return "blend-indices";
        case 3: return "normal";
        case 4: return "uv";
        case 5: return "tangent";
        case 6: return "vertex-color";
        default: return "unknown";
    }
}

/** Returns the serialized size of a vertex attribute type in bytes. */
inline size_t GetAttributeTypeSize(uint32_t type) {
    switch (type) {
        case 1: return 8;
        case 2:
        case 3: return 12;
        case 4: return 8;
        case 8:
        case 9:
        case 10:
        case 11:
        case 12: return 4;
        default: return 0;
    }
}

/** Calculates vertex stride from an attribute declaration list. */
inline size_t ComputeVertexStride(const std::vector<std::pair<uint32_t, uint32_t>>& attributes) {
    size_t stride = 0;
    for (const auto& attribute : attributes) stride += GetAttributeTypeSize(attribute.first);
    return stride;
}

}

#endif
