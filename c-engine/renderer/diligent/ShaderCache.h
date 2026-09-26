#pragma once

#include "Defines.h"

namespace Diligent {
struct IRenderDevice;
struct IShader;
struct IDataBlob;
struct ShaderCreateInfo;
}

namespace engine::renderer::diligent {

Diligent::IShader* shaderCacheCreate(Diligent::IRenderDevice* dev,
                                     Diligent::ShaderCreateInfo& ci,
                                     Diligent::IDataBlob** ppOutput = nullptr);

}
