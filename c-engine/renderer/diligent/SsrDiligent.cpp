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
#include "PostProcess/ScreenSpaceReflection/interface/ScreenSpaceReflection.hpp"

namespace Diligent {
namespace HLSL {
#include "Shaders/Common/public/BasicStructures.fxh"
#include "Shaders/Common/public/ShaderDefinitions.fxh"
#include "Shaders/PostProcess/ScreenSpaceReflection/public/ScreenSpaceReflectionStructures.fxh"
}
}

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

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
std::unique_ptr<ScreenSpaceReflection> ssrEffect;

ITexture*       compositeTex      = nullptr;
ITextureView*   compositeRtv      = nullptr;
IPipelineState* compositePSO      = nullptr;
IShader*        compositeVS       = nullptr;
IShader*        compositePS       = nullptr;
ISampler*       compositeSampler  = nullptr;
RefCntAutoPtr<IShaderResourceBinding> compositeSrb;
ITexture*       compositeSrbScene = nullptr;
ITexture*       compositeSrbSSR   = nullptr;
u32             compositeW        = 0;
u32             compositeH        = 0;

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
    gbufferRtv = nullptr;
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

constexpr char kCompositeVS[] = R"(
struct VSOut
{
    float4 Pos : SV_Position;
    float2 UV : UV;
};

VSOut main(in uint VertId : SV_VertexID)
{
    VSOut Out;
    float2 xy = float2(VertId == 1 ? 3.0 : -1.0, VertId == 2 ? 3.0 : -1.0);
    Out.Pos = float4(xy, 0.0, 1.0);
    Out.UV  = float2((xy.x + 1.0) * 0.5, 1.0 - (xy.y + 1.0) * 0.5);
    return Out;
}
)";

std::string makeCompositePS(float gain) {
    char body[128];
    snprintf(body, sizeof(body),
             "    return float4(scene.rgb + %g * ssr.a * ssr.rgb, scene.a);\n}\n", gain);
    return std::string(
               "Texture2D<float4> g_Scene;\n"
               "SamplerState g_Scene_sampler;\n"
               "Texture2D<float4> g_SSR;\n"
               "\n"
               "struct PSIn\n"
               "{\n"
               "    float4 Pos : SV_Position;\n"
               "    float2 UV : UV;\n"
               "};\n"
               "\n"
               "float4 main(in PSIn In) : SV_Target\n"
               "{\n"
               "    float4 scene = g_Scene.Sample(g_Scene_sampler, In.UV);\n"
               "    float4 ssr = g_SSR.Sample(g_Scene_sampler, In.UV);\n")
           + body;
}

float compositeGain(void) {
    const char* e = getenv("ENGINE_SSR_GAIN");
    if (!e) return 0.5f;
    return (float)atof(e);
}

void createCompositeTexture(u32 w, u32 h) {
    compositeRtv = nullptr;
    if (compositeTex) { compositeTex->Release(); compositeTex = nullptr; }
    compositeSrb = nullptr;
    compositeSrbScene = nullptr;
    compositeSrbSSR   = nullptr;
    compositeW = w;
    compositeH = h;
    TextureDesc desc;
    desc.Name      = "ssrComposite";
    desc.Type      = RESOURCE_DIM_TEX_2D;
    desc.Usage     = USAGE_DEFAULT;
    desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    desc.Format    = TEX_FORMAT_RGBA16_FLOAT;
    desc.Width     = w;
    desc.Height    = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    device->CreateTexture(desc, nullptr, &compositeTex);
    if (!compositeTex) {
        utils::warn("ssr: composite texture creation failed");
        return;
    }
    compositeRtv = compositeTex->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
}

bool initCompositeState(void) {
    ShaderCreateInfo vsCI;
    vsCI.Desc.Name       = "ssrCompositeVS";
    vsCI.Desc.ShaderType = SHADER_TYPE_VERTEX;
    vsCI.EntryPoint      = "main";
    vsCI.SourceLanguage  = SHADER_SOURCE_LANGUAGE_HLSL;
    vsCI.Source          = kCompositeVS;
    device->CreateShader(vsCI, &compositeVS);
    if (!compositeVS) {
        utils::warn("ssr: composite VS compile failed");
        return false;
    }

    const std::string psSrc = makeCompositePS(compositeGain());
    ShaderCreateInfo psCI;
    psCI.Desc.Name       = "ssrCompositePS";
    psCI.Desc.ShaderType = SHADER_TYPE_PIXEL;
    psCI.EntryPoint      = "main";
    psCI.SourceLanguage  = SHADER_SOURCE_LANGUAGE_HLSL;
    psCI.Source          = psSrc.data();
    device->CreateShader(psCI, &compositePS);
    if (!compositePS) {
        utils::warn("ssr: composite PS compile failed");
        return false;
    }

    GraphicsPipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name = "ssrComposite";
    psoCI.pVS          = compositeVS;
    psoCI.pPS          = compositePS;
    GraphicsPipelineDesc& gp = psoCI.GraphicsPipeline;
    gp.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gp.RasterizerDesc.CullMode      = CULL_MODE_NONE;
    gp.DepthStencilDesc.DepthEnable = False;
    gp.NumRenderTargets             = 1;
    gp.RTVFormats[0]                = TEX_FORMAT_RGBA16_FLOAT;
    PipelineResourceLayoutDesc& layout = psoCI.PSODesc.ResourceLayout;
    ShaderResourceVariableDesc vars[3] = {
            {SHADER_TYPE_PIXEL, "g_Scene", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
            {SHADER_TYPE_PIXEL, "g_SSR", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
            {SHADER_TYPE_PIXEL, "g_Scene_sampler", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    layout.Variables    = vars;
    layout.NumVariables = 3;

    device->CreateGraphicsPipelineState(psoCI, &compositePSO);
    if (!compositePSO) {
        utils::warn("ssr: composite PSO creation failed");
        return false;
    }

    SamplerDesc sd;
    sd.MagFilter  = FILTER_TYPE_LINEAR;
    sd.MinFilter  = FILTER_TYPE_LINEAR;
    sd.AddressU   = TEXTURE_ADDRESS_CLAMP;
    sd.AddressV   = TEXTURE_ADDRESS_CLAMP;
    device->CreateSampler(sd, &compositeSampler);
    if (IShaderResourceVariable* v = compositePSO->GetStaticVariableByName(SHADER_TYPE_PIXEL, "g_Scene_sampler")) {
        v->Set(compositeSampler);
    }
    return true;
}

IShaderResourceBinding* compositeSrbFor(ITexture* sceneTex, ITexture* ssrTex) {
    if (!compositeSrb || compositeSrbScene != sceneTex || compositeSrbSSR != ssrTex) {
        RefCntAutoPtr<IShaderResourceBinding> srb;
        compositePSO->CreateShaderResourceBinding(&srb, true);
        if (!srb) {
            utils::warn("ssr: composite SRB creation failed");
            return nullptr;
        }
        if (IShaderResourceVariable* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, "g_Scene")) {
            v->Set(sceneTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        }
        if (IShaderResourceVariable* v = srb->GetVariableByName(SHADER_TYPE_PIXEL, "g_SSR")) {
            v->Set(ssrTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        }
        compositeSrb      = std::move(srb);
        compositeSrbScene = sceneTex;
        compositeSrbSSR   = ssrTex;
    }
    return compositeSrb;
}

} // namespace

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

void ssrDiligentExecute(void) {
    if (!ssrEnabled()) return;
    PostFXContext* pctx = taaPostFXContext();
    if (!pctx) return;
    ITextureView* gbufferSRV = ssrGbufferSRV();
    if (!gbufferSRV) return;

    if (!ssrEffect) {
        ScreenSpaceReflection::CreateInfo ci;
        ci.EnableAsyncCreation = false;
        ssrEffect = std::make_unique<ScreenSpaceReflection>(device, ci);
        if (!ssrEffect) {
            utils::warn("ssr: effect creation failed");
            return;
        }
    }

    taaPostFXExecute(context);
    ssrEffect->PrepareResources(device, context, pctx,
            ScreenSpaceReflection::FEATURE_FLAG_NONE);

    ITextureView* colorSRV   = taaColorSRV();
    ITextureView* depthSRV   = taaDepthSRV(taaFrameIndex());
    ITextureView* motionSRV  = taaMotionSRV();
    if (!colorSRV || !depthSRV || !motionSRV) return;

    HLSL::ScreenSpaceReflectionAttribs attrs{};
    attrs.RoughnessChannel      = 3;
    attrs.IsRoughnessPerceptual = true;

    ScreenSpaceReflection::RenderAttributes ra{};
    ra.pDevice            = device;
    ra.pDeviceContext     = context;
    ra.pPostFXContext     = pctx;
    ra.pColorBufferSRV    = colorSRV;
    ra.pDepthBufferSRV    = depthSRV;
    ra.pNormalBufferSRV   = gbufferSRV;
    ra.pMaterialBufferSRV = gbufferSRV;
    ra.pMotionVectorsSRV  = motionSRV;
    ra.pSSRAttribs        = &attrs;
    ssrEffect->Execute(ra);
}

Diligent::ITextureView* ssrRadianceSRV(void) {
    if (!ssrEnabled() || !ssrEffect) return nullptr;
    return ssrEffect->GetSSRRadianceSRV();
}

Diligent::ITextureView* ssrDiligentComposite(void) {
    if (!ssrEnabled()) return nullptr;
    ITextureView* radianceSRV = ssrRadianceSRV();
    ITextureView* sceneSRV    = taaColorSRV();
    if (!radianceSRV || !sceneSRV) return nullptr;
    if (!compositePSO && !initCompositeState()) return nullptr;
    if (!compositePSO) return nullptr;

    u32 w = 0, h = 0;
    taaTargetSize(&w, &h);
    if (w == 0 || h == 0) return nullptr;
    if (!compositeTex || w != compositeW || h != compositeH) createCompositeTexture(w, h);
    if (!compositeRtv) return nullptr;

    IShaderResourceBinding* srb =
            compositeSrbFor(sceneSRV->GetTexture(), radianceSRV->GetTexture());
    if (!srb) return nullptr;

    context->SetRenderTargets(1, &compositeRtv, nullptr,
                              RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport vp(0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f);
    context->SetViewports(1, &vp, 0, 0);
    Rect scissor(0, 0, (i32)w, (i32)h);
    context->SetScissorRects(1, &scissor, 0, 0);
    context->SetPipelineState(compositePSO);
    context->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DrawAttribs drawAttrs{3, DRAW_FLAG_VERIFY_ALL};
    context->Draw(drawAttrs);
    context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    return compositeTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
}

bool ssrDiligentEnabled(void) {
    return ssrEnabled();
}

void ssrDiligentDestroy(void) {
    if (gbufferPipeline) { gbufferPipeline->Release(); gbufferPipeline = nullptr; }
    if (gbufferPS) { gbufferPS->Release(); gbufferPS = nullptr; }
    gbufferRtv = nullptr;
    if (gbufferTex) { gbufferTex->Release(); gbufferTex = nullptr; }
    gbufferW = 0;
    gbufferH = 0;
    ssrEffect.reset();
    compositeSrb = nullptr;
    compositeSrbScene = nullptr;
    compositeSrbSSR   = nullptr;
    if (compositePSO) { compositePSO->Release(); compositePSO = nullptr; }
    if (compositeVS) { compositeVS->Release(); compositeVS = nullptr; }
    if (compositePS) { compositePS->Release(); compositePS = nullptr; }
    if (compositeSampler) { compositeSampler->Release(); compositeSampler = nullptr; }
    compositeRtv = nullptr;
    if (compositeTex) { compositeTex->Release(); compositeTex = nullptr; }
    compositeW = 0;
    compositeH = 0;
    ssrOn      = false;
    ssrChecked = false;
}
}
