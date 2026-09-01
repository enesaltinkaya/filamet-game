// ktx2bc7: bake a UASTC KTX2 file to raw BC7 blocks in place.
// Uses libktx's transcoder (same basisu engine the game's runtime path uses),
// so output is equivalent to what Terrain.cpp used to do at load time:
//   vkFormat becomes VK_FORMAT_BC7_UNORM_BLOCK (or the sRGB variant for
//   sRGB-encoded sources), supercompression is dropped, mips preserved.
//
// Build (from build-terrain.py, or manually):
//   gcc ktx2bc7.c -I<ktx include> -L<ktx lib> -lktx -o <out>
// Usage:
//   ktx2bc7 <in.ktx2> <out.ktx2>

#include <ktx.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: ktx2bc7 <in.ktx2> <out.ktx2>\n");
        return 2;
    }

    ktxTexture* texture = nullptr;
    KTX_error_code result =
            ktxTexture_CreateFromNamedFile(argv[1], KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture);
    if (result != KTX_SUCCESS) {
        fprintf(stderr, "ktx2bc7: load failed: %s\n", ktxErrorString(result));
        return 1;
    }
    if (texture->classId != ktxTexture2_c) {
        fprintf(stderr, "ktx2bc7: not a KTX2 file: %s\n", argv[1]);
        ktxTexture_Destroy(texture);
        return 1;
    }

    ktxTexture2* ktx2 = (ktxTexture2*)texture;
    if (!ktxTexture2_NeedsTranscoding(ktx2)) {
        fprintf(stderr, "ktx2bc7: not a transcodable (UASTC/BasisLZ) file: %s\n", argv[1]);
        ktxTexture_Destroy(texture);
        return 1;
    }

    result = ktxTexture2_TranscodeBasis(ktx2, KTX_TTF_BC7_RGBA, 0);
    if (result != KTX_SUCCESS) {
        fprintf(stderr, "ktx2bc7: transcode failed: %s\n", ktxErrorString(result));
        ktxTexture_Destroy(texture);
        return 1;
    }

    result = ktxTexture_WriteToNamedFile(texture, argv[2]);
    if (result != KTX_SUCCESS) {
        fprintf(stderr, "ktx2bc7: write failed: %s\n", ktxErrorString(result));
        ktxTexture_Destroy(texture);
        return 1;
    }

    printf("ktx2bc7: %s -> %s (vkFormat %u, %ux%u, %u levels)\n", argv[1], argv[2],
            ktx2->vkFormat, ktx2->baseWidth, ktx2->baseHeight, ktx2->numLevels);
    ktxTexture_Destroy(texture);
    return 0;
}
