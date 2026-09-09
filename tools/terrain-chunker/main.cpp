#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <cctype>
#include <cfloat>
#include <optional>
#include <string>
#include <vector>

#define CGLTF_IMPLEMENTATION
extern "C" {
#include "cgltf.h"
}

#include "meshoptimizer.h"

typedef uint32_t u32;
typedef uint8_t  u8;

static int decompress_meshopt(cgltf_data* data) {
    for (size_t i = 0; i < data->buffer_views_count; i++) {
        if (!data->buffer_views[i].has_meshopt_compression) continue;
        cgltf_meshopt_compression* mc = &data->buffer_views[i].meshopt_compression;
        const unsigned char* src = (const unsigned char*)mc->buffer->data;
        if (!src) return -1;
        src += mc->offset;

        void* buf = malloc(mc->count * mc->stride);
        if (!buf) return -1;

        int rc = -1;
        switch (mc->mode) {
        case cgltf_meshopt_compression_mode_attributes:
            rc = meshopt_decodeVertexBuffer(buf, mc->count, mc->stride, src, mc->size);
            break;
        case cgltf_meshopt_compression_mode_triangles:
            rc = meshopt_decodeIndexBuffer(buf, mc->count, mc->stride, src, mc->size);
            break;
        case cgltf_meshopt_compression_mode_indices:
            rc = meshopt_decodeIndexSequence(buf, mc->count, mc->stride, src, mc->size);
            break;
        default:
            free(buf);
            return -1;
        }
        if (rc != 0) { free(buf); return -1; }

        switch (mc->filter) {
        case cgltf_meshopt_compression_filter_octahedral:
            meshopt_decodeFilterOct(buf, mc->count, mc->stride);
            break;
        case cgltf_meshopt_compression_filter_quaternion:
            meshopt_decodeFilterQuat(buf, mc->count, mc->stride);
            break;
        case cgltf_meshopt_compression_filter_exponential:
            meshopt_decodeFilterExp(buf, mc->count, mc->stride);
            break;
        default:
            break;
        }
        data->buffer_views[i].data = buf;
    }
    return 0;
}

static void free_decoded_meshopt(cgltf_data* data) {
    for (size_t i = 0; i < data->buffer_views_count; i++) {
        if (!data->buffer_views[i].has_meshopt_compression) continue;
        free(data->buffer_views[i].data);
        data->buffer_views[i].data = nullptr;
    }
}

static bool str_has_ci(const char* s, const char* sub) {
    if (!s || !sub) return false;
    size_t sl = strlen(s), su = strlen(sub);
    if (su > sl) return false;
    for (size_t i = 0; i + su <= sl; i++) {
        bool match = true;
        for (size_t j = 0; j < su; j++) {
            if (tolower((unsigned char)s[i + j]) != tolower((unsigned char)sub[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

static std::optional<std::string> extract_extra_raw_object(const char* json, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    const char* p = strstr(json, needle.c_str());
    if (!p) return std::nullopt;
    p += needle.size();
    auto skip_ws = [](const char** pp) {
        while (**pp == ' ' || **pp == '\t' || **pp == '\n' || **pp == '\r') (*pp)++;
    };
    skip_ws(&p);
    if (*p != ':') return std::nullopt;
    p++;
    skip_ws(&p);
    if (*p != '{') return std::nullopt;

    int depth = 0;
    int inStr = 0;
    int esc = 0;
    const char* start = p;
    for (const char* q = p; *q; q++) {
        if (inStr) {
            if (esc) esc = 0;
            else if (*q == '\\') esc = 1;
            else if (*q == '"') inStr = 0;
        } else {
            if (*q == '"') inStr = 1;
            else if (*q == '{') depth++;
            else if (*q == '}' && --depth == 0)
                return std::string(start, (size_t)(q - start + 1));
        }
    }
    return std::nullopt;
}

struct Chunk {
    u32 vert_count = 0;
    u32 idx_count  = 0;
    int has_normals  = 0;
    int has_uvs      = 0;
    int has_tangents = 0;
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<float> tangents;
    std::vector<u32>   idxs;
};

struct TerrainData {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<float> tangents;
    std::vector<u32>   indices;
    int has_normals  = 0;
    int has_uvs      = 0;
    int has_tangents = 0;
};

static void build_chunks(const TerrainData& td, std::vector<Chunk>& chunks,
                          u32 grid_x, u32 grid_y, const float bbMin[3], const float bbMax[3]) {
    float dx = (bbMax[0] - bbMin[0]) / (float)grid_x;
    float dz = (bbMax[2] - bbMin[2]) / (float)grid_y;

    u32 vert_count = (u32)(td.positions.size() / 3);
    u32 tri_count  = (u32)(td.indices.size() / 3);

    std::vector<u8>   in_chunk(vert_count);
    std::vector<u32>  vmap(vert_count);
    std::vector<bool> tri_hit(tri_count);

    for (u32 gx = 0; gx < grid_x; gx++) {
        for (u32 gy = 0; gy < grid_y; gy++) {
            u32 ci = gy * grid_x + gx;
            float x0 = bbMin[0] + (float)gx * dx;
            float x1 = x0 + dx;
            float z0 = bbMin[2] + (float)gy * dz;
            float z1 = z0 + dz;

            std::fill(in_chunk.begin(), in_chunk.end(), 0);
            for (u32 t = 0; t < tri_count; t++) {
                bool hit = false;
                for (u32 v = 0; v < 3; v++) {
                    u32 vi = td.indices[t * 3 + v];
                    float vx = td.positions[vi * 3];
                    float vz = td.positions[vi * 3 + 2];
                    if (vx >= x0 && vx < x1 && vz >= z0 && vz < z1) {
                        hit = true;
                        break;
                    }
                }
                tri_hit[t] = hit;
                if (hit) {
                    in_chunk[td.indices[t * 3]]   = 1;
                    in_chunk[td.indices[t * 3 + 1]] = 1;
                    in_chunk[td.indices[t * 3 + 2]] = 1;
                }
            }

            u32 vc = 0;
            for (u32 vi = 0; vi < vert_count; vi++)
                vmap[vi] = in_chunk[vi] ? (u32)vc++ : (u32)-1;

            u32 ic = 0;
            for (u32 t = 0; t < tri_count; t++)
                if (tri_hit[t]) ic += 3;

            if (vc == 0 || ic == 0) continue;

            Chunk& c = chunks[ci];
            c.vert_count  = vc;
            c.idx_count   = ic;
            c.has_normals  = td.has_normals;
            c.has_uvs      = td.has_uvs;
            c.has_tangents = td.has_tangents;
            c.positions.resize(vc * 3);
            c.idxs.resize(ic);
            if (td.has_normals)  c.normals.resize(vc * 3);
            if (td.has_uvs)      c.uvs.resize(vc * 2);
            if (td.has_tangents) c.tangents.resize(vc * 4);

            for (u32 vi = 0; vi < vert_count; vi++) {
                if (!in_chunk[vi]) continue;
                u32 li = vmap[vi];
                c.positions[li * 3]     = td.positions[vi * 3];
                c.positions[li * 3 + 1] = td.positions[vi * 3 + 1];
                c.positions[li * 3 + 2] = td.positions[vi * 3 + 2];
                if (td.has_normals) {
                    c.normals[li * 3]     = td.normals[vi * 3];
                    c.normals[li * 3 + 1] = td.normals[vi * 3 + 1];
                    c.normals[li * 3 + 2] = td.normals[vi * 3 + 2];
                }
                if (td.has_uvs) {
                    c.uvs[li * 2]     = td.uvs[vi * 2];
                    c.uvs[li * 2 + 1] = td.uvs[vi * 2 + 1];
                }
                if (td.has_tangents) {
                    c.tangents[li * 4]     = td.tangents[vi * 4];
                    c.tangents[li * 4 + 1] = td.tangents[vi * 4 + 1];
                    c.tangents[li * 4 + 2] = td.tangents[vi * 4 + 2];
                    c.tangents[li * 4 + 3] = td.tangents[vi * 4 + 3];
                }
            }

            u32 ic2 = 0;
            for (u32 t = 0; t < tri_count; t++) {
                if (!tri_hit[t]) continue;
                c.idxs[ic2++] = vmap[td.indices[t * 3]];
                c.idxs[ic2++] = vmap[td.indices[t * 3 + 1]];
                c.idxs[ic2++] = vmap[td.indices[t * 3 + 2]];
            }

            printf("  chunk(%u,%u): %u verts, %u idx\n", gx, gy, vc, ic);
        }
    }
}

static void appendf(std::string& s, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    size_t old = s.size();
    s.resize(old + (size_t)n + 1);
    vsnprintf(&s[old], (size_t)n + 1, fmt, ap2);
    s.resize(old + (size_t)n);
    va_end(ap2);
}

static void write_u32_le(FILE* f, u32 v) {
    fputc((u8)(v      ), f);
    fputc((u8)(v >>  8), f);
    fputc((u8)(v >> 16), f);
    fputc((u8)(v >> 24), f);
}

static void write_glb(FILE* f, u32 grid_x, u32 grid_y,
                      const std::vector<Chunk>& chunks, const char* splatInfoRaw) {
    u32 total_chunks = (u32)chunks.size();

    int has_normals = 0, has_uvs = 0, has_tangents = 0;
    for (u32 i = 0; i < total_chunks; i++) {
        if (chunks[i].vert_count == 0) continue;
        has_normals  = chunks[i].has_normals;
        has_uvs      = chunks[i].has_uvs;
        has_tangents = chunks[i].has_tangents;
        break;
    }

    size_t total_pos = 0, total_nrm = 0, total_uv = 0, total_tan = 0, total_idx = 0;
    for (u32 i = 0; i < total_chunks; i++) {
        if (chunks[i].vert_count == 0) continue;
        total_pos += (size_t)chunks[i].vert_count * 3 * sizeof(float);
        if (chunks[i].has_normals)
            total_nrm += (size_t)chunks[i].vert_count * 3 * sizeof(float);
        if (chunks[i].has_uvs)
            total_uv  += (size_t)chunks[i].vert_count * 2 * sizeof(float);
        if (chunks[i].has_tangents)
            total_tan += (size_t)chunks[i].vert_count * 4 * sizeof(float);
        total_idx += (size_t)chunks[i].idx_count * sizeof(u32);
    }

    size_t off_pos = 0;
    size_t off_nrm = total_pos;
    size_t off_uv  = total_pos + total_nrm;
    size_t off_tan = total_pos + total_nrm + total_uv;
    size_t off_idx = total_pos + total_nrm + total_uv + total_tan;
    size_t bin_size = off_idx + total_idx;

    u32 bv_pos = 0, bv_nrm, bv_uv, bv_tan, bv_idx;
    {
        u32 next = 1;
        if (has_normals)  { bv_nrm = next; next++; } else bv_nrm = 0;
        if (has_uvs)      { bv_uv  = next; next++; } else bv_uv = 0;
        if (has_tangents) { bv_tan = next; next++; } else bv_tan = 0;
        bv_idx = next;
    }
    u32 accs_per_chunk = 2 + (has_normals ? 1 : 0) + (has_uvs ? 1 : 0) + (has_tangents ? 1 : 0);

    std::string json;
    json.reserve(256 * 1024);

    appendf(json, "{\"asset\":{\"version\":\"2.0\",\"generator\":\"terrain-chunker\"},");

    appendf(json, "\"bufferViews\":[");
    appendf(json, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962}",
            off_pos, total_pos);
    if (has_normals)
        appendf(json, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962}",
                off_nrm, total_nrm);
    if (has_uvs)
        appendf(json, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962}",
                off_uv, total_uv);
    if (has_tangents)
        appendf(json, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962}",
                off_tan, total_tan);
    appendf(json, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34963}",
            off_idx, total_idx);
    appendf(json, "],");

    appendf(json, "\"accessors\":[");
    {
        size_t p_off = 0, n_off = 0, u_off = 0, t_off = 0, i_off = 0;
        u32 vi = 0;
        for (u32 ci = 0; ci < total_chunks; ci++) {
            if (chunks[ci].vert_count == 0) continue;
            u32 vc = chunks[ci].vert_count;
            u32 ic = chunks[ci].idx_count;

            if (vi > 0) appendf(json, ",");

            appendf(json, "{\"bufferView\":%u,\"byteOffset\":%zu,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"}",
                    bv_pos, p_off, vc);
            if (has_normals)
                appendf(json, ",{\"bufferView\":%u,\"byteOffset\":%zu,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"}",
                        bv_nrm, n_off, vc);
            if (has_uvs)
                appendf(json, ",{\"bufferView\":%u,\"byteOffset\":%zu,\"componentType\":5126,\"count\":%u,\"type\":\"VEC2\"}",
                        bv_uv, u_off, vc);
            if (has_tangents)
                appendf(json, ",{\"bufferView\":%u,\"byteOffset\":%zu,\"componentType\":5126,\"count\":%u,\"type\":\"VEC4\"}",
                        bv_tan, t_off, vc);
            appendf(json, ",{\"bufferView\":%u,\"byteOffset\":%zu,\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"}",
                    bv_idx, i_off, ic);

            p_off += (size_t)vc * 3 * sizeof(float);
            if (has_normals)  n_off += (size_t)vc * 3 * sizeof(float);
            if (has_uvs)      u_off += (size_t)vc * 2 * sizeof(float);
            if (has_tangents) t_off += (size_t)vc * 4 * sizeof(float);
            i_off += (size_t)ic * sizeof(u32);
            vi++;
        }
    }
    appendf(json, "],");

    appendf(json, "\"meshes\":[");
    {
        u32 vi = 0;
        for (u32 ci = 0; ci < total_chunks; ci++) {
            if (chunks[ci].vert_count == 0) continue;
            u32 gx = ci % grid_x;
            u32 gy = ci / grid_x;
            u32 acc_base = vi * accs_per_chunk;
            if (vi) appendf(json, ",");
            appendf(json, "{\"name\":\"terrain_chunk_%u_%u\",\"primitives\":[{\"attributes\":{",
                    gx, gy);
            appendf(json, "\"POSITION\":%u", acc_base);
            if (has_normals)
                appendf(json, ",\"NORMAL\":%u", acc_base + 1);
            u32 off = 1 + (has_normals ? 1 : 0);
            if (has_uvs)
                appendf(json, ",\"TEXCOORD_0\":%u", acc_base + off);
            off += (has_uvs ? 1 : 0);
            if (has_tangents)
                appendf(json, ",\"TANGENT\":%u", acc_base + off);
            off += (has_tangents ? 1 : 0);
            appendf(json, "},\"indices\":%u}]}", acc_base + off);
            vi++;
        }
    }
    appendf(json, "],");

    appendf(json, "\"nodes\":[");
    {
        u32 vi = 0;
        for (u32 ci = 0; ci < total_chunks; ci++) {
            if (chunks[ci].vert_count == 0) continue;
            u32 gx = ci % grid_x;
            u32 gy = ci / grid_x;
            if (vi) appendf(json, ",");
            if (vi == 0 && splatInfoRaw)
                appendf(json, "{\"name\":\"terrain_chunk_%u_%u\",\"mesh\":%u,\"extras\":{\"rigidBodyShape\":\"MESH\",\"splatInfo\":%s}}",
                        gx, gy, vi, splatInfoRaw);
            else
                appendf(json, "{\"name\":\"terrain_chunk_%u_%u\",\"mesh\":%u,\"extras\":{\"rigidBodyShape\":\"MESH\"}}", gx, gy, vi);
            vi++;
        }
    }
    appendf(json, "],");

    appendf(json, "\"scenes\":[{\"nodes\":");
    {
        u32 valid = 0;
        for (u32 ci = 0; ci < total_chunks; ci++)
            if (chunks[ci].vert_count > 0) valid++;
        appendf(json, "[%u", 0);
        for (u32 i = 1; i < valid; i++) appendf(json, ",%u", i);
        appendf(json, "]");
    }
    appendf(json, "}],\"scene\":0,");

    appendf(json, "\"buffers\":[{\"byteLength\":%zu}]}", bin_size);

    size_t json_size = json.size();
    while ((json_size & 3u) != 0) {
        json.push_back(' ');
        json_size++;
    }

    u32 total_size = 28 + (u32)json.size() + (u32)bin_size;

    write_u32_le(f, 0x46546C67);
    write_u32_le(f, 2);
    write_u32_le(f, total_size);

    write_u32_le(f, (u32)json.size());
    write_u32_le(f, 0x4E4F534A);
    fwrite(json.data(), 1, json.size(), f);

    write_u32_le(f, (u32)bin_size);
    write_u32_le(f, 0x004E4942);

    for (u32 i = 0; i < total_chunks; i++) {
        if (chunks[i].vert_count == 0) continue;
        fwrite(chunks[i].positions.data(), sizeof(float), chunks[i].vert_count * 3, f);
    }
    if (has_normals) {
        for (u32 i = 0; i < total_chunks; i++) {
            if (chunks[i].vert_count == 0) continue;
            fwrite(chunks[i].normals.data(), sizeof(float), chunks[i].vert_count * 3, f);
        }
    }
    if (has_uvs) {
        for (u32 i = 0; i < total_chunks; i++) {
            if (chunks[i].vert_count == 0) continue;
            fwrite(chunks[i].uvs.data(), sizeof(float), chunks[i].vert_count * 2, f);
        }
    }
    if (has_tangents) {
        for (u32 i = 0; i < total_chunks; i++) {
            if (chunks[i].vert_count == 0) continue;
            fwrite(chunks[i].tangents.data(), sizeof(float), chunks[i].vert_count * 4, f);
        }
    }
    for (u32 i = 0; i < total_chunks; i++) {
        if (chunks[i].idx_count == 0) continue;
        fwrite(chunks[i].idxs.data(), sizeof(u32), chunks[i].idx_count, f);
    }
}

int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <input.glb> <output.glb> <grid_x> <grid_y>\n", argv[0]);
        return 1;
    }

    const char* input_path  = argv[1];
    const char* output_path = argv[2];
    u32 grid_x = (u32)atoi(argv[3]);
    u32 grid_y = (u32)atoi(argv[4]);

    if (grid_x < 1 || grid_y < 1) {
        fprintf(stderr, "Error: grid dimensions must be >= 1\n");
        return 1;
    }

    cgltf_options options = {};
    cgltf_data* data = nullptr;

    cgltf_result result = cgltf_parse_file(&options, input_path, &data);
    if (result != cgltf_result_success) {
        fprintf(stderr, "Failed to parse %s\n", input_path);
        return 1;
    }
    if (cgltf_load_buffers(&options, data, input_path) != cgltf_result_success) {
        fprintf(stderr, "Failed to load buffers\n");
        cgltf_free(data);
        return 1;
    }
    if (decompress_meshopt(data) != 0) {
        fprintf(stderr, "Failed to decompress meshopt buffers\n");
        free_decoded_meshopt(data);
        cgltf_free(data);
        return 1;
    }

    printf("terrain-chunker: loaded %s (%u meshes, %u nodes)\n",
           input_path, (u32)data->meshes_count, (u32)data->nodes_count);

    int terrain_mesh_idx = -1;
    const cgltf_node* terrain_node = nullptr;

    for (u32 ni = 0; ni < (u32)data->nodes_count; ni++) {
        cgltf_node* node = &data->nodes[ni];
        if (node->mesh && node->name && str_has_ci(node->name, "terrain")) {
            terrain_mesh_idx = (int)(node->mesh - data->meshes);
            terrain_node = node;
            printf("terrain-chunker: terrain at node[%u] \"%s\" -> mesh[%u]\n",
                   ni, node->name, terrain_mesh_idx);
            break;
        }
    }

    if (terrain_mesh_idx < 0) {
        fprintf(stderr, "Error: no terrain mesh found\n");
        free_decoded_meshopt(data);
        cgltf_free(data);
        return 1;
    }

    std::optional<std::string> splatInfoRaw =
        terrain_node->extras.data
            ? extract_extra_raw_object(terrain_node->extras.data, "splatInfo")
            : std::nullopt;
    if (splatInfoRaw)
        printf("terrain-chunker: carrying splatInfo extra (%zu bytes)\n",
               splatInfoRaw->size());

    cgltf_primitive* prim = &data->meshes[terrain_mesh_idx].primitives[0];

    cgltf_accessor* pos_acc = nullptr, *norm_acc = nullptr, *uv_acc = nullptr,
                    *tan_acc = nullptr, *idx_acc = nullptr;
    for (u32 ai = 0; ai < prim->attributes_count; ai++) {
        switch (prim->attributes[ai].type) {
        case cgltf_attribute_type_position: pos_acc = prim->attributes[ai].data; break;
        case cgltf_attribute_type_normal:   norm_acc = prim->attributes[ai].data; break;
        case cgltf_attribute_type_texcoord: uv_acc   = prim->attributes[ai].data; break;
        case cgltf_attribute_type_tangent:  tan_acc  = prim->attributes[ai].data; break;
        default: break;
        }
    }
    idx_acc = prim->indices;

    if (!pos_acc || !idx_acc) {
        fprintf(stderr, "Error: terrain mesh missing POSITION or indices\n");
        free_decoded_meshopt(data);
        cgltf_free(data);
        return 1;
    }

    u32 vert_count = (u32)pos_acc->count;
    u32 idx_count  = (u32)idx_acc->count;

    TerrainData td;
    td.positions.resize(vert_count * 3);
    td.indices.resize(idx_count);
    td.has_normals  = norm_acc != nullptr;
    td.has_uvs      = uv_acc   != nullptr;
    td.has_tangents = tan_acc  != nullptr;
    cgltf_accessor_unpack_floats(pos_acc, td.positions.data(), vert_count * 3);
    cgltf_accessor_unpack_indices(idx_acc, td.indices.data(), sizeof(u32), idx_count);
    if (norm_acc) {
        td.normals.resize(vert_count * 3);
        cgltf_accessor_unpack_floats(norm_acc, td.normals.data(), vert_count * 3);
    }
    if (uv_acc) {
        td.uvs.resize(vert_count * 2);
        cgltf_accessor_unpack_floats(uv_acc, td.uvs.data(), vert_count * 2);
    }
    if (tan_acc) {
        td.tangents.resize(vert_count * 4);
        cgltf_accessor_unpack_floats(tan_acc, td.tangents.data(), vert_count * 4);
    }

    printf("terrain-chunker: terrain %u verts, %u indices\n", vert_count, idx_count);

    float bbMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float bbMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (u32 i = 0; i < vert_count; i++) {
        for (int d = 0; d < 3; d++) {
            float v = td.positions[i * 3 + d];
            if (v < bbMin[d]) bbMin[d] = v;
            if (v > bbMax[d]) bbMax[d] = v;
        }
    }

    printf("terrain-chunker: bbox (%.2f,%.2f,%.2f) -> (%.2f,%.2f,%.2f) grid %ux%u\n",
           bbMin[0], bbMin[1], bbMin[2], bbMax[0], bbMax[1], bbMax[2], grid_x, grid_y);

    std::vector<Chunk> chunks(grid_x * grid_y);

    build_chunks(td, chunks, grid_x, grid_y, bbMin, bbMax);

    u32 valid_count = 0;
    for (u32 i = 0; i < (u32)chunks.size(); i++)
        if (chunks[i].vert_count > 0) valid_count++;

    printf("terrain-chunker: %u/%u valid chunks\n", valid_count, (u32)chunks.size());

    if (valid_count == 0) {
        fprintf(stderr, "Error: no chunks produced\n");
        free_decoded_meshopt(data);
        cgltf_free(data);
        return 1;
    }

    printf("terrain-chunker: writing %s ...\n", output_path);

    FILE* f = fopen(output_path, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot open %s\n", output_path);
        free_decoded_meshopt(data);
        cgltf_free(data);
        return 1;
    }

    write_glb(f, grid_x, grid_y, chunks,
              splatInfoRaw ? splatInfoRaw->c_str() : nullptr);
    fclose(f);

    size_t total_size = 0;
    for (const Chunk& c : chunks) {
        if (c.vert_count == 0) continue;
        total_size += (size_t)c.vert_count * 3 * sizeof(float);
        if (c.has_normals)  total_size += (size_t)c.vert_count * 3 * sizeof(float);
        if (c.has_uvs)      total_size += (size_t)c.vert_count * 2 * sizeof(float);
        if (c.has_tangents) total_size += (size_t)c.vert_count * 4 * sizeof(float);
        total_size += (size_t)c.idx_count * sizeof(u32);
    }

    printf("terrain-chunker: wrote %.1f MB (%u chunks)\n",
           total_size / (1024.0 * 1024.0), valid_count);

    free_decoded_meshopt(data);
    cgltf_free(data);

    return 0;
}
