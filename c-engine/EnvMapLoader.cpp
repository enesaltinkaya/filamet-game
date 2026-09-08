#include "EnvMapLoader.h"

#include "Defines.h"
#include "Utils.h"

#include "stb/git/stb_image.h"

#include <openexr.h>
#include <cstring>

namespace engine {

namespace {

struct ExrMemStream {
    const uint8_t* data;
    uint64_t size;
};

int64_t exrMemRead(exr_const_context_t ctxt, void* userdata, void* buffer, uint64_t sz,
        uint64_t offset, exr_stream_error_func_ptr_t error_cb) {
    (void) ctxt;
    ExrMemStream* s = static_cast<ExrMemStream*>(userdata);
    if (offset >= s->size) {
        if (error_cb)
            error_cb(ctxt, EXR_ERR_READ_IO, "read past end");
        return -1;
    }
    uint64_t avail = s->size - offset;
    if (sz > avail)
        sz = avail;
    memcpy(buffer, s->data + offset, sz);
    return static_cast<int64_t>(sz);
}

int64_t exrMemSize(exr_const_context_t ctxt, void* userdata) {
    (void) ctxt;
    ExrMemStream* s = static_cast<ExrMemStream*>(userdata);
    return static_cast<int64_t>(s->size);
}

}

bool envMapLoadExr(const void* data, size_t dataSize, EnvMapImage& out) {
    ExrMemStream stream = {reinterpret_cast<const uint8_t*>(data), dataSize};

    exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
    init.user_data = &stream;
    init.read_fn = exrMemRead;
    init.size_fn = exrMemSize;

    exr_context_t ctx = nullptr;
    exr_result_t rv = exr_start_read(&ctx, "<memory>", &init);
    if (rv != EXR_ERR_SUCCESS) {
        utils::warn("envmap: exr_start_read failed: %s", exr_get_error_code_as_string(rv));
        return false;
    }

    const int partIdx = 0;

    exr_attr_box2i_t dataWindow;
    rv = exr_get_data_window(ctx, partIdx, &dataWindow);
    if (rv != EXR_ERR_SUCCESS) {
        utils::warn("envmap: exr_get_data_window failed");
        exr_finish(&ctx);
        return false;
    }

    int width = dataWindow.max.x - dataWindow.min.x + 1;
    int height = dataWindow.max.y - dataWindow.min.y + 1;
    if (width <= 0 || height <= 0) {
        utils::warn("envmap: invalid exr dimensions %dx%d", width, height);
        exr_finish(&ctx);
        return false;
    }

    const exr_attr_chlist_t* chlist = nullptr;
    rv = exr_get_channels(ctx, partIdx, &chlist);
    if (rv != EXR_ERR_SUCCESS || !chlist) {
        utils::warn("envmap: exr_get_channels failed");
        exr_finish(&ctx);
        return false;
    }

    int chIdxR = -1, chIdxG = -1, chIdxB = -1, chIdxA = -1;
    for (int i = 0; i < chlist->num_channels; i++) {
        const char* name = chlist->entries[i].name.str;
        if (strcmp(name, "R") == 0)
            chIdxR = i;
        else if (strcmp(name, "G") == 0)
            chIdxG = i;
        else if (strcmp(name, "B") == 0)
            chIdxB = i;
        else if (strcmp(name, "A") == 0)
            chIdxA = i;
    }
    if (chIdxR < 0 || chIdxG < 0 || chIdxB < 0) {
        utils::warn("envmap: missing R/G/B channels");
        exr_finish(&ctx);
        return false;
    }

    u64 pixelCount = static_cast<u64>(width) * height;
    out.pixels.assign(pixelCount * 4, 0.0f);

    int32_t scanlinesPerChunk = 0;
    exr_get_scanlines_per_chunk(ctx, partIdx, &scanlinesPerChunk);
    if (scanlinesPerChunk <= 0)
        scanlinesPerChunk = 1;

    int32_t pixelStride = 4 * static_cast<int32_t>(sizeof(float));
    int32_t lineStride = width * pixelStride;

    int32_t chunkCount = 0;
    exr_get_chunk_count(ctx, partIdx, &chunkCount);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    exr_decode_pipeline_t decoder = EXR_DECODE_PIPELINE_INITIALIZER;
#pragma GCC diagnostic pop
    bool decoderInited = false;

    bool ok = true;
    for (int32_t ci = 0; ci < chunkCount; ci++) {
        exr_chunk_info_t cinfo;
        rv = exr_read_scanline_chunk_info(ctx, partIdx,
                dataWindow.min.y + ci * scanlinesPerChunk, &cinfo);
        if (rv != EXR_ERR_SUCCESS) {
            utils::warn("envmap: chunk info %d failed: %s", ci,
                    exr_get_error_code_as_string(rv));
            ok = false;
            break;
        }

        if (!decoderInited) {
            rv = exr_decoding_initialize(ctx, partIdx, &cinfo, &decoder);
            if (rv != EXR_ERR_SUCCESS) {
                utils::warn("envmap: decoding_initialize failed: %s",
                        exr_get_error_code_as_string(rv));
                ok = false;
                break;
            }
            decoderInited = true;
        } else {
            rv = exr_decoding_update(ctx, partIdx, &cinfo, &decoder);
            if (rv != EXR_ERR_SUCCESS) {
                utils::warn("envmap: decoding_update failed: %s",
                        exr_get_error_code_as_string(rv));
                ok = false;
                break;
            }
        }

        int32_t chunkY0 = cinfo.start_y - dataWindow.min.y;
        uint8_t* base = reinterpret_cast<uint8_t*>(out.pixels.data()) +
                static_cast<u64>(chunkY0) * lineStride;

        for (int16_t ch = 0; ch < decoder.channel_count; ch++) {
            exr_coding_channel_info_t* info = &decoder.channels[ch];
            const char* name = info->channel_name;

            int outOff = -1;
            if (strcmp(name, "R") == 0)
                outOff = 0;
            else if (strcmp(name, "G") == 0)
                outOff = static_cast<int>(sizeof(float));
            else if (strcmp(name, "B") == 0)
                outOff = 2 * static_cast<int>(sizeof(float));
            else if (strcmp(name, "A") == 0)
                outOff = 3 * static_cast<int>(sizeof(float));

            if (outOff >= 0) {
                info->decode_to_ptr = base + outOff;
                info->user_pixel_stride = pixelStride;
                info->user_line_stride = lineStride;
                info->user_bytes_per_element = static_cast<int16_t>(sizeof(float));
                info->user_data_type = static_cast<uint16_t>(EXR_PIXEL_FLOAT);
            } else {
                info->decode_to_ptr = nullptr;
            }
        }

        rv = exr_decoding_choose_default_routines(ctx, partIdx, &decoder);
        if (rv != EXR_ERR_SUCCESS) {
            utils::warn("envmap: choose_default_routines failed: %s",
                    exr_get_error_code_as_string(rv));
            ok = false;
            break;
        }

        rv = exr_decoding_run(ctx, partIdx, &decoder);
        if (rv != EXR_ERR_SUCCESS) {
            utils::warn("envmap: decoding_run chunk %d failed: %s", ci,
                    exr_get_error_code_as_string(rv));
            ok = false;
            break;
        }
    }

    if (decoderInited)
        exr_decoding_destroy(ctx, &decoder);
    exr_finish(&ctx);

    if (!ok) {
        out = EnvMapImage();
        return false;
    }

    if (chIdxA < 0) {
        for (u64 i = 0; i < pixelCount; i++) {
            out.pixels[i * 4 + 3] = 1.0f;
        }
    }

    out.width = width;
    out.height = height;
    return true;
}

bool envMapLoadHdr(const void* data, size_t dataSize, EnvMapImage& out) {
    int w = 0, h = 0, channelsInFile = 0;
    float* pixels = stbi_loadf_from_memory(reinterpret_cast<const stbi_uc*>(data),
            static_cast<int>(dataSize), &w, &h, &channelsInFile, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels)
            free(pixels);
        utils::warn("envmap: failed to decode radiance hdr");
        return false;
    }
    out.width = w;
    out.height = h;
    out.pixels.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    free(pixels);
    return true;
}

bool envMapLoadFromMemory(const void* data, size_t dataSize, const char* path, EnvMapImage& out) {
    const char* dot = path ? strrchr(path, '.') : nullptr;
    if (dot && strcmp(dot, ".exr") == 0) {
        return envMapLoadExr(data, dataSize, out);
    }
    return envMapLoadHdr(data, dataSize, out);
}

}
