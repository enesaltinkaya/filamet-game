#pragma once

namespace Diligent {
struct ITextureView;
}

namespace engine::renderer::diligent {

void ssrDiligentGbufferDraw(void);
Diligent::ITextureView* ssrGbufferSRV(void);
void ssrDiligentDestroy(void);
}
