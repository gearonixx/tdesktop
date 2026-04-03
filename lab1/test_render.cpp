// Quick render test - renders at requested size, checks for crash
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
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
    if (argc < 2) { fprintf(stderr, "Usage: %s <json> [width height]\n", argv[0]); return 1; }

    std::string json_data = read_file(argv[1]);
    auto anim = rlottie::Animation::loadFromData(std::move(json_data), "poc", "", false);
    if (!anim) { fprintf(stderr, "Failed to load\n"); return 1; }

    size_t w = 0, h = 0;
    anim->size(w, h);

    size_t rw = (argc > 2) ? atoi(argv[2]) : w;
    size_t rh = (argc > 3) ? atoi(argv[3]) : h;

    printf("Canvas: %zux%zu, rendering at %zux%zu\n", w, h, rw, rh);

    auto *buf = (uint32_t *)calloc(rw * rh, 4);
    rlottie::Surface surf(buf, rw, rh, rw * 4);

    anim->renderSync(0, surf, true);

    size_t nonzero = 0;
    for (size_t i = 0; i < rw * rh; i++)
        if (buf[i] != 0) nonzero++;
    printf("Done. Non-zero: %zu / %zu (%.1f%%)\n", nonzero, rw * rh,
           100.0 * nonzero / (rw * rh));

    free(buf);
    return 0;
}
