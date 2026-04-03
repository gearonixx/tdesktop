// Quick trace: render poc_v4.json at 1024x1024 and check for non-zero pixels
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <memory>
#include <rlottie.h>

static std::string read_file(const char *path)
{
    std::ifstream f(path);
    if (!f.is_open()) { fprintf(stderr, "Cannot open %s\n", path); exit(1); }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char **argv)
{
    const char *json_path = (argc > 1) ? argv[1] : "poc_v4.json";
    std::string json_data = read_file(json_path);

    auto anim = rlottie::Animation::loadFromData(std::move(json_data), "poc", "", false);
    if (!anim) { fprintf(stderr, "Failed to load\n"); return 1; }

    size_t w = 0, h = 0;
    anim->size(w, h);
    printf("Canvas: %zux%zu, frames: %zu\n", w, h, anim->totalFrame());

    // Render at 2x canvas size to get 1px per 0.5 canvas unit
    size_t rw = 1024, rh = 1024;
    auto *buf = (uint32_t *)calloc(rw * rh, 4);
    rlottie::Surface surf(buf, rw, rh, rw * 4);

    printf("Rendering frame 0 at %zux%zu...\n", rw, rh);
    anim->renderSync(0, surf, true);

    size_t nonzero = 0;
    for (size_t i = 0; i < rw * rh; i++)
        if (buf[i] != 0) nonzero++;
    printf("Non-zero pixels: %zu / %zu (%.1f%%)\n", nonzero, rw * rh,
           100.0 * nonzero / (rw * rh));

    free(buf);
    return 0;
}
