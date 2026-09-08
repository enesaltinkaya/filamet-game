#pragma once

#include "Defines.h"

namespace Diligent {
struct ITexture;
}

namespace engine::renderer::diligent {

void iblDiligentInit(void);
void iblDiligentDestroy(void);

bool iblDiligentReady(void);
Diligent::ITexture* iblDiligentIrradianceCube(void);
Diligent::ITexture* iblDiligentPrefilteredCube(void);
f32 iblDiligentPrefilteredLastMip(void);
const char* iblDiligentEnvName(void);

void iblDiligentCycleNext(void);
void iblDiligentCyclePrev(void);

}
