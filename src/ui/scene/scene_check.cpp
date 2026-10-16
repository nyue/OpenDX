// scene_check FILE...   validate scene files (include/dxuiscene.h) and print
//                       one line per file: meshes, triangles, lights, skipped.
// Exit status 1 if any file is invalid.

#include "SceneFile.h"

#include <cstdio>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: scene_check FILE...\n");
        return 2;
    }
    int bad = 0;
    for (int i = 1; i < argc; ++i) {
        dxui::Scene s;
        std::string error;
        if (!dxui::loadScene(argv[i], s, &error)) {
            std::printf("FAIL %s: %s\n", argv[i], error.c_str());
            ++bad;
            continue;
        }
        std::printf("ok %s meshes=%zu triangles=%zu volumes=%zu lights=%zu skipped=%zu\n",
                    argv[i], s.meshes.size(), s.triangleCount(), s.volumes.size(), s.lights.size(),
                    s.skippedTotal());
    }
    return bad ? 1 : 0;
}
