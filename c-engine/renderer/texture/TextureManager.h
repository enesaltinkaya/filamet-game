#pragma once

#include "Defines.h"
#include "image/Image.h"

namespace Diligent {
struct ITexture;
}

namespace engine {

// Startup prewarm (the old engine's textureManagerInit): list every .ktx2 in
// the paks and decode them in parallel on the default thread pool, blocking
// until the whole batch is done. The GPU upload happens on first use (see
// textureManagerGetTexture) so each texture gets the consumer's srgb flag.
void textureManagerInit(void);

// Cached ITexture for a pak path. Returns an added ref (caller Releases),
// nullptr when the path is missing or failed to load. The flag picks the
// Diligent format for uncompressed 8-bit payloads; the prewarmed decode is
// uploaded with it on the first call for a path.
Diligent::ITexture* textureManagerGetTexture(const char* path, bool srgbForUncompressed);

// Decoded image data for a pak path (weight tiles build a texture array from
// the raw RGBA8). Owned by the manager for its whole lifetime — never
// destroy. nullptr when the path is missing or failed to load.
const utils::Image* textureManagerGetImage(const char* path);

void textureManagerDestroy(void);

}  // namespace engine
