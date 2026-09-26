#pragma once

#include "Defines.h"

#include "Common/interface/RefCntAutoPtr.hpp"

namespace Diligent {
struct IRenderDevice;
struct IShader;
struct IPipelineState;
struct IDataBlob;
struct ShaderCreateInfo;
struct GraphicsPipelineStateCreateInfo;
}

namespace engine::renderer::diligent {

Diligent::IShader* shaderCacheCreate(Diligent::IRenderDevice* dev,
                                     Diligent::ShaderCreateInfo& ci,
                                     Diligent::IDataBlob** ppOutput = nullptr);

}

extern "C" {
Diligent::RefCntAutoPtr<Diligent::IShader> fxShaderCacheCreate(Diligent::IRenderDevice* dev,
                                                               Diligent::ShaderCreateInfo& ci);
Diligent::RefCntAutoPtr<Diligent::IPipelineState> fxPipelineStateCreate(Diligent::IRenderDevice* dev,
                                                                        Diligent::GraphicsPipelineStateCreateInfo& ci);
void fxPipelineStateCreateEx(Diligent::IRenderDevice* dev,
                             Diligent::GraphicsPipelineStateCreateInfo& ci,
                             Diligent::IPipelineState** ppPSO);
}
