#include "renderer/diligent/IblDiligent.h"

#include "EnvMapLoader.h"
#include "Utils.h"
#include "datamanager/DataManager.h"
#include "renderer/diligent/DiligentRenderer.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "DiligentFXShaderSourceStreamFactory.hpp"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/GraphicsTypesX.hpp"
#include "Graphics/GraphicsEngine/interface/PipelineResourceSignature.h"
#include "Graphics/GraphicsEngine/interface/PipelineState.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Sampler.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
#include "Graphics/GraphicsTools/interface/CommonlyUsedStates.h"
#include "Graphics/GraphicsTools/interface/ShaderMacroHelper.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace engine::renderer::diligent {
namespace {

constexpr u32 kIrradianceDim       = 64;
constexpr u32 kPrefilterDim        = 256;
constexpr u32 kPrefilterMips       = 9;
constexpr u32 kBrdfLutDim          = 512;
constexpr u32 kDiffuseSamples      = 2048;
constexpr u32 kSpecularSamples     = 256;
constexpr u32 kBrdfSamples         = 512;
const char*  kEnvDirPrefix        = "images/studiolights/";
const char*  kDefaultEnv          = "kloofendal_48d_partly_cloudy_puresky_1k.exr";

Diligent::RefCntAutoPtr<Diligent::ITexture>      envTex;
Diligent::RefCntAutoPtr<Diligent::ITexture>      envSpecTex;
Diligent::RefCntAutoPtr<Diligent::ITexture>      irradianceCube;
Diligent::RefCntAutoPtr<Diligent::ITexture>      prefilteredCube;
Diligent::RefCntAutoPtr<Diligent::ITexture>      brdfLut;
Diligent::RefCntAutoPtr<Diligent::IPipelineState> irradiancePSO;
Diligent::RefCntAutoPtr<Diligent::IPipelineState> prefilterPSO;
Diligent::RefCntAutoPtr<Diligent::IPipelineState> brdfPSO;
Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> irradianceSRB;
Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> prefilterSRB;
Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> brdfSRB;
std::vector<std::string>               envFiles;
int                                    envIndex     = -1;
std::string                            envName;
bool                                   inited     = false;
bool                                   failed     = false;
engine::EnvMapImage                    lastEnv;

// Global IBL intensity (diffuse + specular; the old engine's
// vulkanIblSetIntensity) and the specular-only attenuation (the old
// engine's IBL_SPEC_INTENSITY). The specular env copy also luminance-clamps
// the env's baked sun disk: the analytic sun already supplies that energy,
// and the disk prefiltered into the spec lobe read as a sheen spike on
// anything reflecting near it.
float iblIntensity         = 1.0f;
float iblSpecularIntensity = 0.35f;
float iblSpecularClamp     = 100.0f;

struct IblPrecomputeAttribs {
    Diligent::float4x4 Rotation;
    Diligent::float4   EnvMapUVScaleBias;
    float              Roughness;
    float              EnvMapWidth;
    float              EnvMapHeight;
    float              EnvMipCount;
    float              EnvMapSlice;
    unsigned int       NumSamples;
    int                SphereMapRow0IsNegativeY;
    unsigned int       Padding;
};

Diligent::IShader* compileFx(const char* name, const char* filePath,
        Diligent::SHADER_TYPE type, const char* entryPoint,
        const Diligent::ShaderMacroHelper& macros) {
    Diligent::ShaderCreateInfo ci;
    ci.Desc.Name = name;
    ci.Desc.ShaderType = type;
    ci.EntryPoint = entryPoint;
    ci.FilePath = filePath;
    ci.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
    ci.Macros = macros;
    ci.CompileFlags = Diligent::SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ci.pShaderSourceStreamFactory = &Diligent::DiligentFXShaderSourceStreamFactory::GetInstance();

    Diligent::RefCntAutoPtr<Diligent::IShader> shader;
    Diligent::RefCntAutoPtr<Diligent::IDataBlob> output;
    device->CreateShader(ci, &shader, &output);
    if (!shader) {
        const char* msg = output ? (const char*)output->GetConstDataPtr() : "(no compiler output)";
        utils::warn("ibl: shader compile failed %s: %s", name, msg);
        return nullptr;
    }
    shader->AddRef();
    return shader;
}

Diligent::ShaderMacroHelper envMapMacros(void) {
    Diligent::ShaderMacroHelper macros;
    macros.AddShaderMacro("OPTIMIZE_SAMPLES", 1);
    macros.AddShaderMacro("ENV_MAP_TYPE_CUBE", 0);
    macros.AddShaderMacro("ENV_MAP_TYPE_SPHERE", 1);
    macros.AddShaderMacro("ENV_MAP_TYPE_SPHERE_ARRAY", 2);
    macros.AddShaderMacro("ENV_MAP_TYPE", 1);
    macros.AddShaderMacro("PREFILTER_ENV_MAP_CHARLIE", 0);
    return macros;
}

bool buildPSOs(void) {
    const Diligent::ShaderMacroHelper macros = envMapMacros();

    Diligent::RefCntAutoPtr<Diligent::IShader> faceVS;
    Diligent::RefCntAutoPtr<Diligent::IShader> irrPS;
    Diligent::RefCntAutoPtr<Diligent::IShader> pfPS;
    Diligent::RefCntAutoPtr<Diligent::IShader> fullVS;
    Diligent::RefCntAutoPtr<Diligent::IShader> brdfPS;
    faceVS = compileFx("ibl cubemap face VS", "CubemapFace.vsh", Diligent::SHADER_TYPE_VERTEX, "main", macros);
    irrPS = compileFx("ibl irradiance PS", "ComputeIrradianceMap.psh", Diligent::SHADER_TYPE_PIXEL, "main", macros);
    pfPS = compileFx("ibl prefilter PS", "PrefilterEnvMap.psh", Diligent::SHADER_TYPE_PIXEL, "main", macros);
    Diligent::ShaderMacroHelper brdfMacros = macros;
    brdfMacros.AddShaderMacro("NUM_SAMPLES", kBrdfSamples);
    fullVS = compileFx("ibl full screen triangle VS", "FullScreenTriangleVS.fx", Diligent::SHADER_TYPE_VERTEX, "FullScreenTriangleVS", brdfMacros);
    brdfPS = compileFx("ibl BRDF LUT PS", "PrecomputeBRDF.psh", Diligent::SHADER_TYPE_PIXEL, "main", brdfMacros);
    if (!faceVS || !irrPS || !pfPS || !fullVS || !brdfPS) {
        return false;
    }

    auto buildCubemapPSO = [&](const char* name, Diligent::IShader* ps,
            Diligent::TEXTURE_FORMAT rtf,
            Diligent::RefCntAutoPtr<Diligent::IPipelineState>& outPSO,
            Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding>& outSRB) -> bool {
        Diligent::GraphicsPipelineStateCreateInfo psoCI;
        Diligent::PipelineStateDesc& psoDesc = psoCI.PSODesc;
        Diligent::GraphicsPipelineDesc& pipe = psoCI.GraphicsPipeline;

        psoDesc.Name = name;
        psoDesc.PipelineType = Diligent::PIPELINE_TYPE_GRAPHICS;
        pipe.NumRenderTargets = 1;
        pipe.RTVFormats[0] = rtf;
        pipe.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        pipe.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
        pipe.DepthStencilDesc.DepthEnable = Diligent::False;

        psoCI.pVS = faceVS;
        psoCI.pPS = ps;

        Diligent::PipelineResourceLayoutDescX layout;
        layout
                .AddVariable(Diligent::SHADER_TYPE_VS_PS, "cbConstants",
                        Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE,
                        Diligent::SHADER_VARIABLE_FLAG_INLINE_CONSTANTS)
                .AddVariable(Diligent::SHADER_TYPE_PIXEL, "g_EnvironmentMap",
                        Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
                .AddImmutableSampler(Diligent::SHADER_TYPE_PIXEL, "g_EnvironmentMap_sampler", Diligent::Sam_LinearClamp);
        psoDesc.ResourceLayout = layout;

        outPSO.Release();
        device->CreateGraphicsPipelineState(psoCI, &outPSO);
        if (!outPSO) {
            utils::warn("ibl: PSO creation failed %s", name);
            return false;
        }
        outPSO->CreateShaderResourceBinding(&outSRB, true);
        return true;
    };

    if (!buildCubemapPSO("IBL irradiance PSO", irrPS, Diligent::TEX_FORMAT_RGBA16_FLOAT,
                    irradiancePSO, irradianceSRB)) {
        return false;
    }
    if (!buildCubemapPSO("IBL prefilter PSO", pfPS, Diligent::TEX_FORMAT_RGBA16_FLOAT,
                    prefilterPSO, prefilterSRB)) {
        return false;
    }

    {
        Diligent::GraphicsPipelineStateCreateInfo psoCI;
        Diligent::PipelineStateDesc& psoDesc = psoCI.PSODesc;
        Diligent::GraphicsPipelineDesc& pipe = psoCI.GraphicsPipeline;

        psoDesc.Name = "IBL BRDF LUT PSO";
        psoDesc.PipelineType = Diligent::PIPELINE_TYPE_GRAPHICS;
        pipe.NumRenderTargets = 1;
        pipe.RTVFormats[0] = Diligent::TEX_FORMAT_RG16_FLOAT;
        pipe.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        pipe.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
        pipe.DepthStencilDesc.DepthEnable = Diligent::False;

        psoCI.pVS = fullVS;
        psoCI.pPS = brdfPS;

        brdfPSO = nullptr;
        device->CreateGraphicsPipelineState(psoCI, &brdfPSO);
        if (!brdfPSO) {
            utils::warn("ibl: BRDF LUT PSO creation failed");
            return false;
        }
        brdfPSO->CreateShaderResourceBinding(&brdfSRB, true);
    }
    return true;
}

void createCubemapTexture(const char* name, Diligent::TEXTURE_FORMAT format, u32 dim,
        u32 mips, Diligent::RefCntAutoPtr<Diligent::ITexture>& out) {
    Diligent::TextureDesc desc;
    desc.Name = name;
    desc.Type = Diligent::RESOURCE_DIM_TEX_CUBE;
    desc.Usage = Diligent::USAGE_DEFAULT;
    desc.BindFlags = Diligent::BIND_SHADER_RESOURCE | Diligent::BIND_RENDER_TARGET;
    desc.Format = format;
    desc.Width = dim;
    desc.Height = dim;
    desc.MipLevels = mips;
    desc.ArraySize = 6;
    out.Release();
    device->CreateTexture(desc, nullptr, &out);
}

template <typename FaceHandlerType>
void processCubemapFaces(Diligent::ITexture* cube, FaceHandlerType&& handler) {
    const Diligent::TextureDesc& desc = cube->GetDesc();
    for (u32 mip = 0; mip < desc.MipLevels; mip++) {
        for (u32 face = 0; face < 6; face++) {
            char name[96];
            snprintf(name, sizeof(name), "RTV face %u mip %u of %s", (unsigned)face,
                    (unsigned)mip, desc.Name ? desc.Name : "");
            Diligent::TextureViewDesc rtvDesc{name, Diligent::TEXTURE_VIEW_RENDER_TARGET,
                    Diligent::RESOURCE_DIM_TEX_2D_ARRAY};
            rtvDesc.MostDetailedMip = mip;
            rtvDesc.FirstArraySlice = face;
            rtvDesc.NumArraySlices = 1;
            Diligent::RefCntAutoPtr<Diligent::ITextureView> rtv;
            cube->CreateView(rtvDesc, &rtv);
            if (!rtv) {
                utils::warn("ibl: RTV creation failed for %s face %u mip %u",
                        desc.Name ? desc.Name : "", (unsigned)face, (unsigned)mip);
                return;
            }
            context->SetRenderTargets(1, (Diligent::ITextureView*[]) {rtv}, nullptr,
                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            handler(rtv, mip, face);
        }
    }
}

void clearCubemap(Diligent::ITexture* cube) {
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    processCubemapFaces(cube, [&black](Diligent::ITextureView* rtv, u32, u32) {
        context->ClearRenderTarget(rtv, black, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    });
}

bool createEnvTexture(const char* name, const engine::EnvMapImage& img,
        Diligent::RefCntAutoPtr<Diligent::ITexture>& out) {
    const u32 width = (u32)img.width;
    const u32 height = (u32)img.height;
    u32 mipLevels = 1;
    u32 maxDim = width > height ? width : height;
    while (maxDim > 1) {
        maxDim >>= 1;
        mipLevels++;
    }

    std::vector<u8> zero;
    std::vector<Diligent::TextureSubResData> subres(mipLevels);
    {
        size_t total = 0;
        for (u32 m = 1; m < mipLevels; m++) {
            u32 w = std::max(1u, width >> m);
            u32 h = std::max(1u, height >> m);
            total += (size_t)w * h * 16;
        }
        zero.assign(total, 0);
    }
    size_t zeroOff = 0;
    for (u32 m = 0; m < mipLevels; m++) {
        u32 w = std::max(1u, width >> m);
        u32 h = std::max(1u, height >> m);
        if (m == 0) {
            subres[m] = Diligent::TextureSubResData((const void*)img.pixels.data(), (size_t)width * 16);
        } else {
            subres[m] = Diligent::TextureSubResData(zero.data() + zeroOff, (size_t)w * 16);
            zeroOff += (size_t)w * h * 16;
        }
    }
    Diligent::TextureData data;
    data.pSubResources = subres.data();
    data.NumSubresources = mipLevels;
    data.pContext = context;

    Diligent::TextureDesc desc;
    desc.Name = name;
    desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    desc.Usage = Diligent::USAGE_DEFAULT;
    desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    desc.Format = Diligent::TEX_FORMAT_RGBA32_FLOAT;
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = mipLevels;

    if (out) {
        if (irradianceSRB)
            Diligent::ShaderResourceVariableX{irradianceSRB, Diligent::SHADER_TYPE_PIXEL, "g_EnvironmentMap"}.Set(nullptr);
        if (prefilterSRB)
            Diligent::ShaderResourceVariableX{prefilterSRB, Diligent::SHADER_TYPE_PIXEL, "g_EnvironmentMap"}.Set(nullptr);
        out.Release();
    }

    Diligent::RefCntAutoPtr<Diligent::ITexture> tex;
    device->CreateTexture(desc, &data, &tex);
    if (!tex) {
        utils::warn("ibl: env texture creation failed %s", name);
        return false;
    }
    context->GenerateMips(tex->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
    Diligent::StateTransitionDesc barrier{tex, Diligent::RESOURCE_STATE_UNKNOWN,
            Diligent::RESOURCE_STATE_SHADER_RESOURCE, Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE};
    context->TransitionResourceState(barrier);
    out = tex.Detach();
    return true;
}

bool uploadEnvironment(const engine::EnvMapImage& img) {
    return createEnvTexture("IBL environment equirect", img, envTex);
}

// The specular IBL (prefiltered env) reads a scaled copy of the environment:
// the diffuse (irradiance) pass keeps full strength, only the spec lobe is
// attenuated (the old engine's IBL_SPEC_INTENSITY, applied to the prefilter
// source so no per-frame uniform is needed). The copy also luminance-clamps
// the env's baked sun disk: with the analytic sun aligned ~8 deg from the
// disk, the prefiltered spec picked up a double-counted sun sheen (measured
// 4.7x the sans-disk value at roughness 0.54).
bool uploadSpecEnvironment(void) {
    engine::EnvMapImage scaled;
    scaled.width  = lastEnv.width;
    scaled.height = lastEnv.height;
    scaled.pixels.resize(lastEnv.pixels.size());
    for (size_t i = 0; i < lastEnv.pixels.size(); i += 4) {
        f32 r = lastEnv.pixels[i];
        f32 g = lastEnv.pixels[i + 1];
        f32 b = lastEnv.pixels[i + 2];
        if (iblSpecularClamp > 0.0f) {
            const f32 lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            if (lum > iblSpecularClamp) {
                const f32 k = iblSpecularClamp / lum;
                r *= k;
                g *= k;
                b *= k;
            }
        }
        scaled.pixels[i]     = r * iblSpecularIntensity;
        scaled.pixels[i + 1] = g * iblSpecularIntensity;
        scaled.pixels[i + 2] = b * iblSpecularIntensity;
        scaled.pixels[i + 3] = lastEnv.pixels[i + 3];
    }
    return createEnvTexture("IBL environment equirect (specular)", scaled, envSpecTex);
}

// +X -X +Y -Y +Z -Z
static const Diligent::float4x4 kFaceMats[6] = {
    Diligent::float4x4::RotationY(-Diligent::PI_F / 2.0f),
    Diligent::float4x4::RotationY(+Diligent::PI_F / 2.0f),
    Diligent::float4x4::RotationX(+Diligent::PI_F / 2.0f),
    Diligent::float4x4::RotationX(-Diligent::PI_F / 2.0f),
    Diligent::float4x4::Identity(),
    Diligent::float4x4::RotationY(-Diligent::PI_F),
};

void runEnvPass(Diligent::IPipelineState* pso, Diligent::IShaderResourceBinding* srb,
        Diligent::ITexture* cube, unsigned numSamples, bool perMipRoughness,
        Diligent::ITexture* envSrc) {
    const Diligent::TextureDesc& envDesc = envSrc->GetDesc();
    context->SetPipelineState(pso);
    Diligent::ITextureView* srv = envSrc->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    Diligent::ShaderResourceVariableX{ srb, Diligent::SHADER_TYPE_PIXEL, "g_EnvironmentMap" }.Set(srv);
    context->CommitShaderResources(srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Diligent::ShaderResourceVariableX cb{srb, Diligent::SHADER_TYPE_VERTEX, "cbConstants"};
    processCubemapFaces(cube, [&cb, cube, numSamples, perMipRoughness, &envDesc](
                    Diligent::ITextureView*, u32 mip, u32 face) {
        IblPrecomputeAttribs a;
        a.Rotation = kFaceMats[face];
        a.EnvMapUVScaleBias = {1.0f, 1.0f, 0.0f, 0.0f};
        a.Roughness = perMipRoughness && cube->GetDesc().MipLevels > 1 ?
                (float)mip / (float)(cube->GetDesc().MipLevels - 1) : 0.0f;
        a.EnvMapWidth = (float)envDesc.Width;
        a.EnvMapHeight = (float)envDesc.Height;
        a.EnvMipCount = (float)envDesc.MipLevels;
        a.EnvMapSlice = 0.0f;
        a.NumSamples = numSamples;
        a.SphereMapRow0IsNegativeY = 0;
        a.Padding = 0;
        cb.SetInlineConstants(&a, 0, sizeof(a) / sizeof(Diligent::Uint32));
        context->Draw(Diligent::DrawAttribs{4, Diligent::DRAW_FLAG_VERIFY_ALL});
    });
    Diligent::ShaderResourceVariableX{ srb, Diligent::SHADER_TYPE_PIXEL, "g_EnvironmentMap" }.Set(nullptr);
}

// Re-run the prefilter pass (reads envSpecTex, the specular-scaled env).
void recomputePrefilter(void) {
    if (!envSpecTex || !prefilteredCube) {
        return;
    }
    runEnvPass(prefilterPSO, prefilterSRB, prefilteredCube, kSpecularSamples, true, envSpecTex);
    Diligent::StateTransitionDesc barrier{prefilteredCube, Diligent::RESOURCE_STATE_UNKNOWN,
            Diligent::RESOURCE_STATE_SHADER_RESOURCE, Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE};
    context->TransitionResourceState(barrier);
}

void precomputeCubemaps(void) {
    runEnvPass(irradiancePSO, irradianceSRB, irradianceCube, kDiffuseSamples, false, envTex);
    recomputePrefilter();

    context->SetPipelineState(brdfPSO);
    Diligent::ITextureView* lutRtv = brdfLut->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
    context->SetRenderTargets(1, (Diligent::ITextureView*[]) {lutRtv}, nullptr,
            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->Draw(Diligent::DrawAttribs{3, Diligent::DRAW_FLAG_VERIFY_ALL});

    Diligent::StateTransitionDesc barriers[3] = {
        {irradianceCube, Diligent::RESOURCE_STATE_UNKNOWN, Diligent::RESOURCE_STATE_SHADER_RESOURCE,
                Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE},
        {prefilteredCube, Diligent::RESOURCE_STATE_UNKNOWN, Diligent::RESOURCE_STATE_SHADER_RESOURCE,
                Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE},
        {brdfLut, Diligent::RESOURCE_STATE_UNKNOWN, Diligent::RESOURCE_STATE_SHADER_RESOURCE,
                Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE},
    };
    context->TransitionResourceStates(3, barriers);
}

bool loadEnvironment(const char* path) {
    utils::String blob = utils::dataManagerRead(path);
    if (!blob.data || blob.size == 0) {
        utils::warn("ibl: failed to read %s", path);
        return false;
    }
    engine::EnvMapImage img;
    bool ok = engine::envMapLoadFromMemory(blob.data, blob.size, path, img);
    utils::stringDestroy(&blob);
    if (!ok || img.pixels.empty()) {
        utils::warn("ibl: failed to decode %s", path);
        return false;
    }

    if (!uploadEnvironment(img)) {
        return false;
    }
    lastEnv = img;
    if (!uploadSpecEnvironment()) {
        return false;
    }
    precomputeCubemaps();

    const char* slash = strrchr(path, '/');
    envName = slash ? slash + 1 : path;
    utils::info("ibl: loaded %s (%dx%d, %u diffuse / %u specular samples)", path,
            img.width, img.height, (unsigned)kDiffuseSamples, (unsigned)kSpecularSamples);
    return true;
}

void buildEnvFileList(void) {
    for (const char* ext : {".exr", ".hdr"}) {
        std::vector<utils::String> files = utils::dataManagerListFiles(ext);
        for (utils::String& s : files) {
            if (s.size > strlen(kEnvDirPrefix) &&
                    strncmp(s.data, kEnvDirPrefix, strlen(kEnvDirPrefix)) == 0) {
                envFiles.push_back(std::string(s.data));
            }
            utils::stringDestroy(&s);
        }
    }
    std::sort(envFiles.begin(), envFiles.end());
    utils::info("ibl: found %zu environment maps in %s", envFiles.size(), kEnvDirPrefix);
}

void selectDefaultEnv(void) {
    envIndex = 0;
    const char* env = getenv("ENGINE_IBL_ENV");
    if (env && env[0]) {
        for (size_t i = 0; i < envFiles.size(); i++) {
            const std::string& p = envFiles[i];
            const char* base = p.c_str() + strlen(kEnvDirPrefix);
            size_t dot = p.find_last_of('.');
            std::string stem = dot != std::string::npos ? p.substr(0, dot) : p;
            if (strcmp(base, env) == 0 || stem == env) {
                envIndex = (int)i;
                return;
            }
        }
        utils::warn("ibl: ENGINE_IBL_ENV '%s' not found, using %s", env, kDefaultEnv);
    }
    const std::string defPath = std::string(kEnvDirPrefix) + kDefaultEnv;
    for (size_t i = 0; i < envFiles.size(); i++) {
        if (envFiles[i] == defPath) {
            envIndex = (int)i;
            return;
        }
    }
}

}

void iblDiligentInit(void) {
    if (inited) {
        return;
    }
    inited = true;
    if (const char* v = getenv("ENGINE_IBL_INTENSITY")) {
        iblIntensity = (f32)atof(v);
    }
    if (const char* v = getenv("ENGINE_IBL_SPEC_INTENSITY")) {
        iblSpecularIntensity = (f32)atof(v);
    }
    if (const char* v = getenv("ENGINE_IBL_SPEC_CLAMP")) {
        iblSpecularClamp = (f32)atof(v);
    }
    if (iblIntensity < 0.0f) {
        iblIntensity = 0.0f;
    }
    if (iblSpecularIntensity < 0.0f) {
        iblSpecularIntensity = 0.0f;
    }
    if (!device || !context) {
        utils::warn("ibl: no device/context, IBL stays off");
        failed = true;
        return;
    }

    if (!buildPSOs()) {
        utils::warn("ibl: pipeline build failed, IBL stays off");
        failed = true;
        return;
    }

    createCubemapTexture("IBL irradiance cube", Diligent::TEX_FORMAT_RGBA16_FLOAT,
            kIrradianceDim, 1, irradianceCube);
    createCubemapTexture("IBL prefiltered environment", Diligent::TEX_FORMAT_RGBA16_FLOAT,
            kPrefilterDim, kPrefilterMips, prefilteredCube);
    {
        Diligent::TextureDesc desc;
        desc.Name = "IBL preintegrated BRDF LUT";
        desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
        desc.Usage = Diligent::USAGE_DEFAULT;
        desc.BindFlags = Diligent::BIND_SHADER_RESOURCE | Diligent::BIND_RENDER_TARGET;
        desc.Format = Diligent::TEX_FORMAT_RG16_FLOAT;
        desc.Width = kBrdfLutDim;
        desc.Height = kBrdfLutDim;
        desc.MipLevels = 1;
        device->CreateTexture(desc, nullptr, &brdfLut);
    }
    if (!irradianceCube || !prefilteredCube || !brdfLut) {
        utils::warn("ibl: output texture creation failed, IBL stays off");
        failed = true;
        return;
    }
    clearCubemap(irradianceCube);
    clearCubemap(prefilteredCube);
    {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        Diligent::ITextureView* rtv = brdfLut->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
        context->ClearRenderTarget(rtv, black, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }

    buildEnvFileList();
    if (envFiles.empty()) {
        utils::warn("ibl: no environment maps found in %s, IBL stays off", kEnvDirPrefix);
        failed = true;
        return;
    }
    selectDefaultEnv();
    if (!loadEnvironment(envFiles[envIndex].c_str())) {
        failed = true;
        return;
    }
}

void iblDiligentDestroy(void) {
    if (!inited) {
        return;
    }
    inited = false;
    if (irradianceSRB) {
        Diligent::ShaderResourceVariableX{irradianceSRB, Diligent::SHADER_TYPE_PIXEL, "g_EnvironmentMap"}.Set(nullptr);
    }
    if (prefilterSRB) {
        Diligent::ShaderResourceVariableX{prefilterSRB, Diligent::SHADER_TYPE_PIXEL, "g_EnvironmentMap"}.Set(nullptr);
    }
    irradianceSRB.Release();
    prefilterSRB.Release();
    brdfSRB.Release();
    irradiancePSO.Release();
    prefilterPSO.Release();
    brdfPSO.Release();
    envTex.Release();
    envSpecTex.Release();
    lastEnv = engine::EnvMapImage();
    irradianceCube.Release();
    prefilteredCube.Release();
    brdfLut.Release();
    envFiles.clear();
    envIndex = -1;
    envName.clear();
    failed = false;
}

bool iblDiligentReady(void) {
    return inited && !failed && irradianceCube && prefilteredCube && envTex;
}

Diligent::ITexture* iblDiligentIrradianceCube(void) {
    return iblDiligentReady() ? irradianceCube : nullptr;
}

Diligent::ITexture* iblDiligentPrefilteredCube(void) {
    return iblDiligentReady() ? prefilteredCube : nullptr;
}

f32 iblDiligentPrefilteredLastMip(void) {
    return (f32)(kPrefilterMips - 1);
}

const char* iblDiligentEnvName(void) {
    return envName.c_str();
}

// Global IBL intensity (scales the diffuse + specular IBL together; applied
// through each path's IBLScale).
f32 iblDiligentGetIntensity(void) {
    return iblIntensity;
}

void iblDiligentSetIntensity(f32 intensity) {
    if (intensity < 0.0f) {
        intensity = 0.0f;
    }
    iblIntensity = intensity;
}

// Specular-only IBL attenuation (the old engine's IBL_SPEC_INTENSITY): scales
// the prefiltered environment the spec lobe samples, leaving the diffuse
// irradiance at full strength. Changing it re-runs the prefilter pass.
f32 iblDiligentGetSpecularIntensity(void) {
    return iblSpecularIntensity;
}

void iblDiligentSetSpecularIntensity(f32 intensity) {
    if (intensity < 0.0f) {
        intensity = 0.0f;
    }
    if (intensity == iblSpecularIntensity) {
        return;
    }
    iblSpecularIntensity = intensity;
    if (!inited || failed || !iblDiligentReady() || lastEnv.pixels.empty()) {
        return;
    }
    if (!uploadSpecEnvironment()) {
        return;
    }
    recomputePrefilter();
}

void iblDiligentCycleNext(void) {
    if (!inited || failed || envFiles.size() < 2) {
        return;
    }
    envIndex = (envIndex + 1) % (int)envFiles.size();
    loadEnvironment(envFiles[envIndex].c_str());
}

void iblDiligentCyclePrev(void) {
    if (!inited || failed || envFiles.size() < 2) {
        return;
    }
    envIndex = (envIndex + (int)envFiles.size() - 1) % (int)envFiles.size();
    loadEnvironment(envFiles[envIndex].c_str());
}

}
