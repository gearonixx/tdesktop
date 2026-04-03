
#include <cstdio>

#ifdef HAVE_RLOTTIE
#include "rlottie.h"
#include <vector>

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "poc.json";
    auto anim = rlottie::Animation::loadFromFile(path);
    if (!anim) {
        fprintf(stderr, "Failed to load: %s\n", path);
        return 1;
    }

    size_t w = 0, h = 0;
    anim->size(w, h);
    printf("Loaded: %s  size=%zux%zu  frames=%zu\n",
           path, w, h, anim->totalFrame());
    std::vector<uint32_t> buf(w * h);
    rlottie::Surface surface(buf.data(), w, h, w * sizeof(uint32_t));
    anim->renderSync(0, surface);  

    printf("rendered frame 0\n");
    printf("rebuild with -fsanitize=address to detect the overflow.\n");
    return 0;
}
#else
int main()
{
    


    return 0;
}
#endif
