/**
 * @file main.cpp
 * @brief Use case using BullyMSH_iOS_Android.h
 *
 *
 * Usage:
 *   main.exe <input.obj> <output.msh>              OBJ to MSH
 *   main.exe <input.msh> <output.obj>              MSH to OBJ
 *   main.exe parse <input.msh> <output.txt>        Parse MSH only
 *   main.exe skin  <input.msh> <donor.msh> <out.msh> (transfer skin weights)
 */

#include "BullyMSH_iOS_Android.h"

#include <fstream>
#include <iostream>


// Parse MSH

static bool ParseMsh(const BullyMSH::MeshData& mesh,
                           const std::filesystem::path& mshPath,
                           const std::filesystem::path& txtPath) {
    std::ofstream out(txtPath);
    if (!out) {
        std::cerr << "Cannot parse file: " << txtPath.string() << "\n";
        return false;
    }

    out << " File : " << mshPath.filename().string() << "\n";
    out << "[CONTAINER]\n";
    out << "  Format version  : " << mesh.version << "\n";
    out << "  Model name      : " << mesh.name << "\n";
    out << "  Vertices        : " << mesh.vertices.size() << "\n";
    out << "  Indices         : " << mesh.indices.size() << "\n";
    out << "  Triangles       : " << mesh.indices.size() / 3 << "\n\n";

    out << "[MATERIALS] (" << mesh.materials.size() << ")\n";
    for (size_t i = 0; i < mesh.materials.size(); ++i) {
        out << "  [" << i << "] " << mesh.materials[i] << "\n";
    }
    out << "\n";

    out << "[BONES] (" << mesh.bones.size() << ")\n";
    const std::vector<int> order = BullyMSH::BuildBoneOrder(mesh);
    const std::vector<size_t> usage = BullyMSH::BuildBoneUsageTable(mesh);
    for (int index : order) {
        const BullyMSH::Bone& bone = mesh.bones[index];
        out << "  [" << index << "] " << bone.name
            << "  parent=" << bone.parent
            << "  depth=" << BullyMSH::GetBoneDepth(mesh, index)
            << "  influenced=" << usage[index] << "\n";
    }
    out << "\n";

    out << "[BOUNDS]\n";
    out << "  Low  : (" << mesh.low.x << ", " << mesh.low.y << ", " << mesh.low.z << ")\n";
    out << "  High : (" << mesh.high.x << ", " << mesh.high.y << ", " << mesh.high.z << ")\n";
    out << "  Size : (" << (mesh.high.x - mesh.low.x) << ", "
        << (mesh.high.y - mesh.low.y) << ", " << (mesh.high.z - mesh.low.z) << ")\n\n";

    out << "[SKIN WEIGHTS]\n";
    const size_t shown = mesh.vertices.size() < 10 ? mesh.vertices.size() : 10;
    for (size_t i = 0; i < shown; ++i) {
        const BullyMSH::Vertex& v = mesh.vertices[i];
        out << "  v" << i << " sum=" << BullyMSH::GetTotalBlendWeight(v)
            << "  bones=[";
        for (int slot = 0; slot < 4; ++slot) {
            out << static_cast<int>(v.blendIndices[slot]) << ":"
                << static_cast<int>(v.blendWeights[slot]);
            if (slot < 3) out << " ";
        }
        out << "]\n";
    }
    if (mesh.vertices.size() > shown) {
        out << "  ... (" << mesh.vertices.size() - shown << " more vertices)\n";
    }
    out << "\n";

    std::string validationError;
    const bool valid = BullyMSH::ValidateMesh(mesh, validationError);
    out << "[VALIDATION] " << (valid ? "PASS" : "FAIL") << "\n";
    if (!valid) out << "  Error: " << validationError << "\n";
    std::cout << "File exported " << txtPath.string() << "\n";
    return true;
}

static bool LoadAny(const std::filesystem::path& path, BullyMSH::MeshData& mesh) {
    std::string err;
    const std::string ext = BullyMSH::Internal::LowerExtension(path);
    bool ok = false;
    if (ext == ".obj") {
        std::cout << "Loading OBJ: " << path.string() << "\n";
        ok = BullyMSH::LoadObj(path, mesh, err);
    } else if (ext == ".msh") {
        std::cout << "Loading MSH: " << path.string() << "\n";
        ok = BullyMSH::LoadMsh(path, mesh, err);
    } else {
        err = "Unsupported input extension: " + ext;
    }
    if (!ok) {
        std::cerr << "Error: " << err << "\n";
        return false;
    }

    std::string validationError;
    if (!BullyMSH::ValidateMesh(mesh, validationError)) {
        std::cerr << "Validation warning: " << validationError << "\n";
    }
    return true;
}

static void PrintUsage() {
    std::cout << "  main.exe <input.obj> <output.msh>                 OBJ to MSH\n";
    std::cout << "  main.exe <input.msh> <output.obj>                 MSH to OBJ\n";
    std::cout << "  main.exe parse <input.msh> <output.txt>           Parse MSH\n";
    std::cout << "  main.exe skin <input.msh> <donor.msh> <out.msh>    transfer skin weights\n";
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 3) {
            PrintUsage();
            return 1;
        }

        const std::string mode = argv[1];

        // parse 
        if (mode == "parse") {
            if (argc < 4) {
                std::cout << "Usage: main.exe parse <input.msh> <output.txt>\n";
                return 1;
            }
            BullyMSH::MeshData mesh;
            if (!LoadAny(argv[2], mesh)) return 1;
            return ParseMsh(mesh, argv[2], argv[3]) ? 0 : 1;
        }

        // skin transfer
        if (mode == "skin") {
            if (argc < 5) {
                std::cout << "Usage: main.exe skin <input.msh> <donor.msh> <out.msh>\n";
                return 1;
            }
            BullyMSH::MeshData mesh;
            BullyMSH::MeshData donor;
            if (!LoadAny(argv[2], mesh)) return 1;
            if (!LoadAny(argv[3], donor)) return 1;

            const BullyMSH::SkinStats stats = BullyMSH::TransferSkin(mesh, donor);
            std::cout << "Skin transferred: avg distance " << stats.averageDistance
                      << ", max distance " << stats.maximumDistance << "\n";

            BullyMSH::ExportOptions options;
            std::string err;
            if (!BullyMSH::SaveMsh(argv[4], mesh, options, err)) {
                std::cerr << "Error: " << err << "\n";
                return 1;
            }
            std::cout << "Skinned MSH written to " << argv[4] << "\n";
            return 0;
        }

        // convert 
        std::filesystem::path input(argv[1]);
        std::filesystem::path output(argv[2]);

        BullyMSH::MeshData mesh;
        if (!LoadAny(input, mesh)) return 1;

        const std::string extOut = BullyMSH::Internal::LowerExtension(output);
        std::string err;

        if (extOut == ".msh") {
            BullyMSH::ExportOptions options;
            if (!BullyMSH::SaveMsh(output, mesh, options, err)) {
                std::cerr << "Error: " << err << "\n";
                return 1;
            }
        } else if (extOut == ".obj") {
            if (!BullyMSH::SaveObj(output, mesh, err)) {
                std::cerr << "Error: " << err << "\n";
                return 1;
            }
        } else {
            std::cerr << "Unsupported output extension: " << extOut << "\n";
            return 1;
        }

        std::cout << "Done. " << mesh.vertices.size() << " vertices, "
                  << mesh.indices.size() / 3 << " triangles -> " << output.string() << "\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Fatal exception: " << e.what() << "\n";
        return 1;
    }
}
