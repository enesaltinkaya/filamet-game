#include "renderer/diligent/SsrDiligent.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "renderer/diligent/DiligentRenderer.h"
#include "renderer/diligent/HeightmapTerrainDiligent.h"
#include "renderer/diligent/TaaDiligent.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "DiligentFXShaderSourceStreamFactory.hpp"
#include "Graphics/GraphicsEngine/interface/Buffer.h"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/PipelineState.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Shader.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
#include "Graphics/GraphicsTools/interface/ShaderMacroHelper.hpp"

#include <cstdio>

namespace engine::renderer::diligent {
using namespace Diligent;
using engine::renderer::diligent::device;
using engine::renderer::diligent::context;

namespace {

IPipelineState* gbufferPipeline = nullptr;
IShader*        gbufferPS       = nullptr;
ITexture*       gbufferTex      = nullptr;
ITextureView*   gbufferRtv      = nullptr;
u32             gbufferW        = 0;
u32             gbufferH        = 0;
bool            ssrOn           = false;
bool            ssrChecked      = false;

bool ssrEnabled(void) {
    if (!ssrChecked) {
        ssrChecked = true;
        ssrOn = getenv("ENGINE_SSR") != nullptr;
    }
    return ssrOn;
}

IShader* createGbufferPS(void) {
    utils::String blob = utils::dataManagerRead("materials/heightmap_terrain_ps.hlsl");
    if (!blob.data || blob.size == 0) {
        utils::warn("ssr: shader source missing from pak: materials/heightmap_terrain_ps.hlsl");
        utils::stringDestroy(&blob);
        return nullptr;
    }
    ShaderMacroHelper macros;
    macros.AddShaderMacro("GBUFFER_OUTPUT", 1);
    ShaderCreateInfo ci;
    ci.Desc.Name        = "heightmapTerrainGbufferPS";
    ci.Desc.ShaderType  = SHADER_TYPE_PIXEL;
    ci.EntryPoint       = "main";
    ci.Source           = blob.data;
    ci.SourceLength     = blob.size;
    ci.SourceLanguage   = SHADER_SOURCE_LANGUAGE_HLSL;
    ci.Macros           = macros;
    ci.pShaderSourceStreamFactory = &DiligentFXShaderSourceStreamFactory::GetInstance();

    RefCntAutoPtr<IShader> shader;
    RefCntAutoPtr<IDataBlob> output;
    device->CreateShader(ci, &shader, &output);
    utils::stringDestroy(&blob);
    if (!shader) {
        const char* msg = output ? (const char*)output->GetConstDataPtr() : "(no compiler output)";
        utils::warn("ssr: gbuffer PS compile failed: %s", msg);
        return nullptr;
    }
    shader->AddRef();
    return shader;
}

void createGbufferTexture(u32 w, u32 h) {
    if (gbufferRtv) { gbufferRtv->Release(); gbufferRtv = nullptr; }
    if (gbufferTex) { gbufferTex->Release(); gbufferTex = nullptr; }
    gbufferW = w;
    gbufferH = h;
    TextureDesc desc;
    desc.Name      = "ssrGbuffer";
    desc.Type      = RESOURCE_DIM_TEX_2D;
    desc.Usage     = USAGE_DEFAULT;
    desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    desc.Format    = TEX_FORMAT_RGBA16_FLOAT;
    desc.Width     = w;
    desc.Height    = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    device->CreateTexture(desc, nullptr, &gbufferTex);
    if (!gbufferTex) {
        utils::warn("ssr: gbuffer texture creation failed");
        return;
    }
    gbufferRtv = gbufferTex->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
}

bool initGbufferState(void) {
    if (!heightmapTerrainDiligentPRS() || !heightmapTerrainDiligentLitVS()) return false;
    gbufferPS = createGbufferPS();
    if (!gbufferPS) return false;

    IPipelineResourceSignature* prs =
            (IPipelineResourceSignature*)heightmapTerrainDiligentPRS();
    GraphicsPipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name       = "ssrGbufferTerrain";
    psoCI.ppResourceSignatures    = &prs;
    psoCI.ResourceSignaturesCount = 1;

    GraphicsPipelineDesc& gp = psoCI.GraphicsPipeline;
    gp.RasterizerDesc.FrontCounterClockwise = true;
    gp.RasterizerDesc.CullMode               = CULL_MODE_BACK;
    gp.DepthStencilDesc.DepthEnable         = true;
    gp.DepthStencilDesc.DepthWriteEnable    = false;
    gp.DepthStencilDesc.DepthFunc           = COMPARISON_FUNC_LESS_EQUAL;
    gp.PrimitiveTopology                    = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gp.NumRenderTargets                     = 1;
    gp.RTVFormats[0]                        = TEX_FORMAT_RGBA16_FLOAT;
    gp.DSVFormat                            = TEX_FORMAT_D32_FLOAT;

    static const LayoutElement inputLayout[] = {
            {"ATTRIB", 0, 0, 3, VT_FLOAT32, False, 0u,  24u},
            {"ATTRIB", 1, 0, 3, VT_FLOAT32, False, 12u, 24u},
    };
    gp.InputLayout.LayoutElements = inputLayout;
    gp.InputLayout.NumElements    = 2;

    psoCI.pVS = (IShader*)heightmapTerrainDiligentLitVS();
    psoCI.pPS = gbufferPS;
    device->CreateGraphicsPipelineState(psoCI, &gbufferPipeline);
    if (!gbufferPipeline) {
        utils::warn("ssr: gbuffer PSO creation failed");
        return false;
    }
    return true;
}

}

void ssrDiligentGbufferDraw(void) {
    if (!ssrEnabled()) return;
    if (!gbufferPipeline && !initGbufferState()) return;
    if (!gbufferPipeline) return;

    u32 w = 0, h = 0;
    taaTargetSize(&w, &h);
    if (w == 0 || h == 0) return;
    if (w != gbufferW || h != gbufferH) createGbufferTexture(w, h);

    ITextureView* dsv = taaDepthDSV();
    if (!gbufferRtv || !dsv) return;

    ITextureView* rtv = gbufferRtv;
    context->SetRenderTargets(1, &rtv, dsv, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport vp(0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f);
    context->SetViewports(1, &vp, 0, 0);
    Rect scissor(0, 0, (i32)w, (i32)h);
    context->SetScissorRects(1, &scissor, 0, 0);
    const float clear[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    context->ClearRenderTarget(rtv, clear, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    heightmapTerrainDiligentGbufferDrawTiles(gbufferPipeline);
}

Diligent::ITextureView* ssrGbufferSRV(void) {
    if (!ssrEnabled() || !gbufferTex) return nullptr;
    return gbufferTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
}

void ssrDiligentDestroy(void) {
    fprintf(stderr, "ssrdbg: destroy p=%p ps=%p rtv=%p tex=%p\n", (void*)gbufferPipeline, (void*)gbufferPS, (void*)gbufferRtv, (void*)gbufferTex);
    if (gbufferPipeline) { gbufferPipeline->Release(); gbufferPipeline = nullptr; }
    fprintf(stderr, "ssrdbg: pipeline released\n");
    if (gbufferPS) { gbufferPS->Release(); gbufferPS = nullptr; }
    fprintf(stderr, "ssrdbg: ps released\n");
    if (gbufferRtv) { gbufferRtv->Release(); gbufferRtv = nullptr; }
    fprintf(stderr, "ssrdbg: rtv released\n");
    if (gbufferTex) { gbufferTex->Release(); gbufferTex = nullptr; }
    fprintf(stderr, "ssrdbg: tex released\n");
    gbufferW = 0;
    gbufferH = 0;
    ssrOn      = false;
    ssrChecked = false;
}
}
