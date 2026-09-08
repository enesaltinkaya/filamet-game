#pragma once
#include "Common/interface/BasicMath.hpp"

#include <cmath>

namespace engine::renderer::diligent {

struct FrustumCullPlanes {
    float p[6][4];
};

inline void frustumCullFromVP(const Diligent::float4x4& vp, bool glNdc, FrustumCullPlanes& out) {
    const float* m = &vp._11;
    float c[4][4];
    for (int j = 0; j < 4; j++) {
        for (int k = 0; k < 4; k++) {
            c[j][k] = m[k * 4 + j];
        }
    }
    for (int i = 0; i < 4; i++) {
        const int lat = i >> 1;
        const float s = (i == 0 || i == 2) ? 1.0f : -1.0f;
        for (int k = 0; k < 4; k++) {
            out.p[i][k] = c[3][k] + s * c[lat][k];
        }
    }
    for (int k = 0; k < 4; k++) {
        out.p[4][k] = glNdc ? c[3][k] + c[2][k] : c[2][k];
        out.p[5][k] = c[3][k] - c[2][k];
    }
    for (int i = 0; i < 6; i++) {
        const float len = std::sqrt(out.p[i][0] * out.p[i][0] +
                out.p[i][1] * out.p[i][1] + out.p[i][2] * out.p[i][2]);
        if (len > 1e-8f) {
            for (int k = 0; k < 4; k++) {
                out.p[i][k] /= len;
            }
        }
    }
}

inline bool frustumCullAabbOutside(const float bmin[3], const float bmax[3],
        const FrustumCullPlanes& pl) {
    for (int i = 0; i < 6; i++) {
        const float* p = pl.p[i];
        const float px = p[0] >= 0.0f ? bmax[0] : bmin[0];
        const float py = p[1] >= 0.0f ? bmax[1] : bmin[1];
        const float pz = p[2] >= 0.0f ? bmax[2] : bmin[2];
        if (p[0] * px + p[1] * py + p[2] * pz + p[3] < 0.0f) return true;
    }
    return false;
}

}
