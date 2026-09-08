#pragma once

namespace Diligent {
struct ITextureView;
}

namespace engine::renderer::diligent {

void ssrDiligentGbufferDraw(void);
Diligent::ITextureView* ssrGbufferSRV(void);
bool ssrDiligentEnabled(void);
void ssrDiligentExecute(void);
Diligent::ITextureView* ssrRadianceSRV(void);
Diligent::ITextureView* ssrDiligentComposite(void);
void ssrDiligentDestroy(void);
}
