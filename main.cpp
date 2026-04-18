#include "plugin.h"
#include "CClouds.h"
#include "CCamera.h"
#include "CGame.h"
#include "CGeneral.h"
#include "CWeather.h"
#include "CTimeCycle.h"
#include "CTimer.h"

#include <math.h>
#include <algorithm>
#include <fstream>
#include <string>
#include <cstdlib>

using namespace plugin;

namespace {
    static const int   kMaxProcClouds = 512;

    struct CloudSettings {
        int   targetProcClouds = 160;
        float minSpawnZ = 200.0f;
        float spawnRadiusMin = 1500.0f;
        float spawnRadiusMax = 2200.0f;
        float renderMaxDist = 2200.0f;
        float respawnMaxDist = 2800.0f;
        float windMoveScale = 0.90f;
        float minSpacing2D = 45.0f;
        float spawnFadeInSpeed = 0.02f;
        float quadFacingMin = 0.20f;
    };

    struct ProcCloud {
        bool         used;
        CVector      pos;
        CVector      size;
        unsigned char alpha;
        float        fadeIn;
    };

    struct RenderCloudEntry {
        int   index;
        float dist;
    };

    static ProcCloud gProcClouds[kMaxProcClouds];
    static bool gCloudPoolInitialized = false;
    static bool gWasOutsideLastFrame = false;
    static CVector gCloudWorldCenter;
    static CloudSettings gCfg;

    static std::string Trim(const std::string& s) {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
        return s.substr(b, e - b);
    }

    static void WriteDefaultIni(const char* path) {
        std::ofstream out(path);
        if (!out.is_open())
            return;
        out << "; VolumetricCloudsExtended settings\n";
        out << "TargetProcClouds=" << gCfg.targetProcClouds << "\n";
        out << "MinSpawnZ=" << gCfg.minSpawnZ << "\n";
        out << "SpawnRadiusMin=" << gCfg.spawnRadiusMin << "\n";
        out << "SpawnRadiusMax=" << gCfg.spawnRadiusMax << "\n";
        out << "RenderMaxDist=" << gCfg.renderMaxDist << "\n";
        out << "RespawnMaxDist=" << gCfg.respawnMaxDist << "\n";
        out << "WindMoveScale=" << gCfg.windMoveScale << "\n";
        out << "MinSpacing2D=" << gCfg.minSpacing2D << "\n";
        out << "SpawnFadeInSpeed=" << gCfg.spawnFadeInSpeed << "\n";
        out << "QuadFacingMin=" << gCfg.quadFacingMin << "\n";
    }

    static void LoadIni() {
        const char* iniPath = "VolumetricCloudsExtended.ini";
        std::ifstream in(iniPath);
        if (!in.is_open()) {
            WriteDefaultIni(iniPath); // generate once; user can tune afterwards
            return;
        }

        std::string line;
        while (std::getline(in, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#')
                continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = Trim(line.substr(0, eq));
            const std::string val = Trim(line.substr(eq + 1));
            const float f = (float)atof(val.c_str());

            if (key == "TargetProcClouds") gCfg.targetProcClouds = (int)f;
            else if (key == "MinSpawnZ") gCfg.minSpawnZ = f;
            else if (key == "SpawnRadiusMin") gCfg.spawnRadiusMin = f;
            else if (key == "SpawnRadiusMax") gCfg.spawnRadiusMax = f;
            else if (key == "RenderMaxDist") gCfg.renderMaxDist = f;
            else if (key == "RespawnMaxDist") gCfg.respawnMaxDist = f;
            else if (key == "WindMoveScale") gCfg.windMoveScale = f;
            else if (key == "MinSpacing2D") gCfg.minSpacing2D = f;
            else if (key == "SpawnFadeInSpeed") gCfg.spawnFadeInSpeed = f;
            else if (key == "QuadFacingMin") gCfg.quadFacingMin = f;
        }

        if (gCfg.targetProcClouds < 1) gCfg.targetProcClouds = 1;
        if (gCfg.targetProcClouds > kMaxProcClouds) gCfg.targetProcClouds = kMaxProcClouds;
        if (gCfg.spawnRadiusMin < 1.0f) gCfg.spawnRadiusMin = 1.0f;
        if (gCfg.spawnRadiusMax < gCfg.spawnRadiusMin + 1.0f) gCfg.spawnRadiusMax = gCfg.spawnRadiusMin + 1.0f;
        if (gCfg.respawnMaxDist < gCfg.renderMaxDist + 50.0f) gCfg.respawnMaxDist = gCfg.renderMaxDist + 50.0f;
        if (gCfg.minSpacing2D < 1.0f) gCfg.minSpacing2D = 1.0f;
        if (gCfg.spawnFadeInSpeed < 0.001f) gCfg.spawnFadeInSpeed = 0.001f;
        if (gCfg.quadFacingMin < 0.0f) gCfg.quadFacingMin = 0.0f;
        if (gCfg.quadFacingMin > 1.0f) gCfg.quadFacingMin = 1.0f;
    }

    static float Dist2DSq(const CVector& a, const CVector& b) {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return dx * dx + dy * dy;
    }

    static CVector NormalizeSafe(const CVector& v) {
        const float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        if (len <= 0.0001f)
            return CVector(0.0f, 0.0f, 0.0f);
        const float invLen = 1.0f / len;
        return CVector(v.x * invLen, v.y * invLen, v.z * invLen);
    }

    static CVector MakeSpawnPos(const CVector& centerPos) {
        const float angle = CGeneral::GetRandomNumberInRange(0.0f, 6.2831853f);
        const float rad = CGeneral::GetRandomNumberInRange(gCfg.spawnRadiusMin, gCfg.spawnRadiusMax);
        const float zMin = std::max(gCfg.minSpawnZ, centerPos.z - 40.0f);
        const float zMax = std::max(zMin + 40.0f, centerPos.z + 220.0f);
        const float z = CGeneral::GetRandomNumberInRange(zMin, zMax);
        return CVector(
            centerPos.x + cosf(angle) * rad,
            centerPos.y + sinf(angle) * rad,
            z
        );
    }

    static void RespawnCloud(ProcCloud& c, const CVector& centerPos) {
        const float rand1To5 = CGeneral::GetRandomNumberInRange(1.0f, 5.0f);
        const float minSize = rand1To5 * 20.0f;
        const float maxSizeXY = rand1To5 * 100.0f;
        const float maxSizeZ = rand1To5 * 40.0f;

        c.used = true;
        c.pos = MakeSpawnPos(centerPos);
        c.size = CVector(
            CGeneral::GetRandomNumberInRange(minSize, maxSizeXY),
            CGeneral::GetRandomNumberInRange(minSize, maxSizeXY),
            CGeneral::GetRandomNumberInRange(minSize, maxSizeZ)
        );
        c.alpha = (unsigned char)CGeneral::GetRandomNumberInRange(36.0f, 128.0f);
        c.fadeIn = 0.0f;
    }

    static void ResetCloudPool() {
        for (int i = 0; i < kMaxProcClouds; i++) {
            gProcClouds[i].used = false;
            gProcClouds[i].alpha = 0;
            gProcClouds[i].size = CVector(0.0f, 0.0f, 0.0f);
            gProcClouds[i].fadeIn = 0.0f;
        }
        gCloudPoolInitialized = false;
    }

    static void EnsureCloudPoolInitialized() {
        if (gCloudPoolInitialized)
            return;

        // Make sure GTA SA's original volumetric cloud model-space tables are initialized.
        CClouds::VolumetricCloudsInit();

        const CVector camPos = TheCamera.GetPosition();
        gCloudWorldCenter = camPos;

        for (int i = 0; i < kMaxProcClouds; i++) {
            gProcClouds[i].used = false;
            gProcClouds[i].pos = camPos;
            gProcClouds[i].size = CVector(0.0f, 0.0f, 0.0f);
            gProcClouds[i].alpha = 0;
            gProcClouds[i].fadeIn = 0.0f;
        }

        for (int i = 0; i < gCfg.targetProcClouds; i++) {
            RespawnCloud(gProcClouds[i], gCloudWorldCenter);
        }

        gCloudPoolInitialized = true;
    }

    static void UpdateProceduralClouds() {
        if (!CGame::CanSeeOutSideFromCurrArea()) {
            gWasOutsideLastFrame = false;
            ResetCloudPool();
            return;
        }

        EnsureCloudPoolInitialized();

        const CVector camPos = TheCamera.GetPosition();
        if (!gWasOutsideLastFrame) {
            gCloudWorldCenter = camPos;
            for (int i = 0; i < gCfg.targetProcClouds; i++) {
                RespawnCloud(gProcClouds[i], gCloudWorldCenter);
            }
        }
        gWasOutsideLastFrame = true;

        // Re-anchor spawn center around the current camera position once we travel far away.
        if (Dist2DSq(gCloudWorldCenter, camPos) > (gCfg.renderMaxDist * 0.6f) * (gCfg.renderMaxDist * 0.6f)) {
            gCloudWorldCenter = camPos;
        }
        // Keep a stable world-space cloud origin so clouds don't visually rotate/follow camera yaw.
        // (Only wind and cloud simulation move them after initial spawn.)

        const CVector2D windDir = CWeather::WindDir;
        const float windStep = CWeather::Wind * CTimer::ms_fTimeStep * gCfg.windMoveScale;

        int activeCount = 0;
        for (int i = 0; i < kMaxProcClouds; i++) {
            ProcCloud& c = gProcClouds[i];
            if (!c.used)
                continue;

            c.pos.x += windDir.x * windStep;
            c.pos.y += windDir.y * windStep;
            c.fadeIn = std::min(1.0f, c.fadeIn + CTimer::ms_fTimeStep * gCfg.spawnFadeInSpeed);

            if (c.pos.z < gCfg.minSpawnZ)
                c.pos.z = gCfg.minSpawnZ;

            const float cloudExtent = std::max(c.size.x, std::max(c.size.y, c.size.z));
            const float nearDist2D = std::max(0.0f, sqrtf(Dist2DSq(c.pos, camPos)) - cloudExtent);
            if (nearDist2D > gCfg.respawnMaxDist) {
                RespawnCloud(c, gCloudWorldCenter);
            }
            else {
                activeCount++;
            }
        }

        for (int i = 0; i < kMaxProcClouds && activeCount < gCfg.targetProcClouds; i++) {
            ProcCloud& c = gProcClouds[i];
            if (!c.used) {
                RespawnCloud(c, gCloudWorldCenter);
                activeCount++;
            }
        }

        if ((CTimer::m_snTimeInMilliseconds & 3) == 0) {
            const float minDistSq = gCfg.minSpacing2D * gCfg.minSpacing2D;
            for (int i = 0; i < kMaxProcClouds; i++) {
                if (!gProcClouds[i].used)
                    continue;

                for (int j = i + 1; j < kMaxProcClouds; j++) {
                    if (!gProcClouds[j].used)
                        continue;

                    CVector& a = gProcClouds[i].pos;
                    CVector& b = gProcClouds[j].pos;

                    const float dx = b.x - a.x;
                    const float dy = b.y - a.y;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 < 0.0001f || d2 >= minDistSq)
                        continue;

                    const float d = sqrtf(d2);
                    const float push = (gCfg.minSpacing2D - d) * 0.5f;
                    const float nx = dx / d;
                    const float ny = dy / d;

                    a.x -= nx * push;
                    a.y -= ny * push;
                    b.x += nx * push;
                    b.y += ny * push;
                }
            }
        }
    }

    void __cdecl RenderProceduralClouds() {
        CClouds::m_bVolumetricCloudHeightSwitch = false;
        *reinterpret_cast<float*>(0xC6E970) = 0.0f;

        if (!CGame::CanSeeOutSideFromCurrArea())
            return;

        RwTexture* cloudTex = CClouds::ms_vc.m_pTex;
        if (!cloudTex) {
            plugin::Call<0x716380>();
            return;
        }

        void* oldZWrite = 0;
        void* oldZTest = 0;
        void* oldVertexAlpha = 0;
        void* oldSrcBlend = 0;
        void* oldDstBlend = 0;
        void* oldRaster = 0;
        void* oldFog = 0;
        void* oldCull = 0;
        void* oldTexFilter = 0;
        RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, &oldZWrite);
        RwRenderStateGet(rwRENDERSTATEZTESTENABLE, &oldZTest);
        RwRenderStateGet(rwRENDERSTATEVERTEXALPHAENABLE, &oldVertexAlpha);
        RwRenderStateGet(rwRENDERSTATESRCBLEND, &oldSrcBlend);
        RwRenderStateGet(rwRENDERSTATEDESTBLEND, &oldDstBlend);
        RwRenderStateGet(rwRENDERSTATETEXTURERASTER, &oldRaster);
        RwRenderStateGet(rwRENDERSTATEFOGENABLE, &oldFog);
        RwRenderStateGet(rwRENDERSTATECULLMODE, &oldCull);
        RwRenderStateGet(rwRENDERSTATETEXTUREFILTER, &oldTexFilter);

        RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
        RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)TRUE);
        RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE);
        RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
        RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, RwTextureGetRaster(cloudTex));
        RwRenderStateSet(rwRENDERSTATEFOGENABLE, oldFog);
        RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)rwCULLMODECULLNONE);
        RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*)rwFILTERLINEAR);

        const CVector camPos = TheCamera.GetPosition();
        const float fadeOutBeginDist = gCfg.renderMaxDist - 120.0f;
        const float fadeOutEndDist = gCfg.respawnMaxDist;

        RenderCloudEntry drawList[kMaxProcClouds];
        int drawCount = 0;
        for (int i = 0; i < kMaxProcClouds; i++) {
            const ProcCloud& c = gProcClouds[i];
            if (!c.used)
                continue;
            drawList[drawCount].index = i;
            drawList[drawCount].dist = (c.pos - camPos).Magnitude();
            drawCount++;
        }

        std::sort(drawList, drawList + drawCount, [](const RenderCloudEntry& a, const RenderCloudEntry& b) {
            return a.dist > b.dist; // render farthest to nearest for proper blending
            });

        for (int n = 0; n < drawCount; n++) {
            const ProcCloud& c = gProcClouds[drawList[n].index];

            const float dist = drawList[n].dist;
            const float cloudExtent = std::max(c.size.x, std::max(c.size.y, c.size.z));
            const float nearDist = std::max(0.0f, dist - cloudExtent);
            int cloudAlpha = (int)c.alpha;
            if (nearDist > fadeOutBeginDist) {
                if (nearDist > fadeOutEndDist)
                    continue;
                const float t = (fadeOutEndDist - nearDist) / (fadeOutEndDist - fadeOutBeginDist);
                const int distAlpha = (int)(std::max(0.0f, t) * cloudAlpha);
                cloudAlpha = std::min(cloudAlpha, distAlpha);
            }
            const float spawnFade = c.fadeIn * c.fadeIn;
            cloudAlpha = (int)(cloudAlpha * spawnFade);

            const unsigned char alpha = (unsigned char)cloudAlpha;
            if (alpha < 6)
                continue;

            const auto& cc = CTimeCycle::m_CurrentColours;
            const unsigned char vcClr = (unsigned char)std::min(
                ((int)cc.m_nSkyTopRed + (int)cc.m_nSkyTopGreen + (int)cc.m_nSkyTopBlue +
                    (int)cc.m_nSkyBottomRed + (int)cc.m_nSkyBottomGreen + (int)cc.m_nSkyBottomBlue) / 6 + 64,
                255
            );

            const CVector vcToCamDir = NormalizeSafe(c.pos - camPos);

            // Reuse GTA SA volumetric cloud visual layout from CClouds::VolumetricCloudsRender:
            // 18 vertices = 3 quads * 2 tris. Submit in one batch to avoid per-triangle seams.
            RwIm3DVertex v[18];
            for (int k = 0; k < 18; k++) {
                const int quadIdx = k / 6;
                const float quadFacingRaw = fabsf(DotProduct(vcToCamDir, CClouds::ms_vc.m_vecCloudsSpace[quadIdx]));
                const float quadFacing = std::max(gCfg.quadFacingMin, quadFacingRaw);
                const unsigned char quadAlpha = (unsigned char)(quadFacing * alpha);

                const CVector p = c.pos + CVector(
                    CClouds::ms_vc.m_fCloudXCoords[k] * c.size.x,
                    CClouds::ms_vc.m_fCloudYCoords[k] * c.size.y,
                    CClouds::ms_vc.m_fCloudZCoords[k] * c.size.z
                );
                RwIm3DVertexSetPos(&v[k], p.x, p.y, p.z);
                RwIm3DVertexSetU(&v[k], CClouds::ms_vc.m_fCloudUCoords[k]);
                RwIm3DVertexSetV(&v[k], CClouds::ms_vc.m_fCloudVCoords[k]);
                RwIm3DVertexSetRGBA(&v[k], vcClr, vcClr, vcClr, quadAlpha);
            }

            if (RwIm3DTransform(v, 18, NULL, rwIM3D_VERTEXUV | rwIM3D_VERTEXRGBA)) {
                RwIm3DRenderPrimitive(rwPRIMTYPETRILIST);
                RwIm3DEnd();
            }
        }

        RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, oldVertexAlpha);
        RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, oldZWrite);
        RwRenderStateSet(rwRENDERSTATEZTESTENABLE, oldZTest);
        RwRenderStateSet(rwRENDERSTATESRCBLEND, oldSrcBlend);
        RwRenderStateSet(rwRENDERSTATEDESTBLEND, oldDstBlend);
        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, oldRaster);
        RwRenderStateSet(rwRENDERSTATEFOGENABLE, oldFog);
        RwRenderStateSet(rwRENDERSTATECULLMODE, oldCull);
        RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, oldTexFilter);
    }

    class VolumetricCloudsExtended {
    public:
        VolumetricCloudsExtended() {
            LoadIni();
            patch::RedirectCall(0x53E1B4, RenderProceduralClouds);
            Events::gameProcessEvent += UpdateProceduralClouds;
        }
    };
}

VolumetricCloudsExtended gVolumetricCloudsExtended;
