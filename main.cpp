#include "plugin.h"
#include "CClouds.h"
#include "CCamera.h"
#include "CGame.h"
#include "CGeneral.h"
#include "CWeather.h"
#include "CTimeCycle.h"
#include "CTimer.h"
#include "CTxdStore.h"

#include <math.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace plugin;

namespace {
    static const int kMaxProcClouds = 512;
    static const int kMaxCloudTextures = 16;

    struct CloudSettings {
        int   targetProcClouds = 160;
        float badWeatherDensityMultiplier = 1.40f;
        float minSpawnZ = 200.0f;
        float spawnRadiusMin = 1500.0f;
        float spawnRadiusMax = 2200.0f;
        float renderMaxDist = 2200.0f;
        float respawnMaxDist = 2800.0f;
        float windMoveScale = 0.90f;
        float minSpacing2D = 45.0f;
        float spawnFadeInSpeed = 0.02f;
        float quadFacingMin = 0.20f;
        float farClipFadeRange = 450.0f;
        float farClipFadeMargin = 80.0f;
        float timecycFarClipScale = 1.80f;
        bool  useTimecycTint = true;
        float timecycTintStrength = 1.0f;
        float fluffyCloudBottomBlend = 0.0f;
        int   colorBrightnessBoost = 64;
        bool  useAdditionalTextures = true;
        bool  includeOriginalTextureInRandom = true;
    };

    struct ProcCloud {
        bool         used;
        CVector      pos;
        CVector      size;
        unsigned char alpha;
        float        fadeIn;
        unsigned char textureIndex;
    };

    struct RenderCloudEntry {
        int   index;
        float dist;
    };

    struct CloudRenderColour {
        unsigned char r;
        unsigned char g;
        unsigned char b;
    };

    struct CloudTextureEntry {
        RwTexture* texture;
        bool       ownedByPlugin;
    };

    static ProcCloud gProcClouds[kMaxProcClouds];
    static bool gCloudPoolInitialized = false;
    static bool gWasOutsideLastFrame = false;
    static CVector gCloudWorldCenter;
    static CloudSettings gCfg;

    static std::string gRequestedTextureNames[kMaxCloudTextures - 1] = { "cloud2", "cloud3", "cloud4" };
    static int gRequestedTextureNameCount = 3;
    static CloudTextureEntry gCloudTextures[kMaxCloudTextures];
    static int gCloudTextureCount = 0;
    static bool gCloudTexturesInitialized = false;
    static RwTexture* gOriginalTextureAtLastLoad = nullptr;

    static std::string Trim(const std::string& s) {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
        return s.substr(b, e - b);
    }

    static bool ParseBool(const std::string& val) {
        return val == "1" || val == "true" || val == "True" || val == "TRUE" ||
            val == "yes" || val == "Yes" || val == "YES" || val == "on" || val == "On" || val == "ON";
    }

    static RwTexture* ReadRwTexture(const char* name, const char* maskName) {
        // GTA SA 1.0 US RenderWare texture read function. Calling the game code directly avoids
        // linker dependency on plugin-sdk RenderWare.cpp in generated single-file projects.
        return plugin::CallAndReturn<RwTexture*, 0x7F3AC0, const char*, const char*>(name, maskName);
    }

    static void DestroyRwTexture(RwTexture* texture) {
        if (texture)
            plugin::CallAndReturn<RwBool, 0x7F3820, RwTexture*>(texture);
    }

    static void ParseTextureNameList(const std::string& val) {
        gRequestedTextureNameCount = 0;
        size_t start = 0;
        while (start <= val.size() && gRequestedTextureNameCount < kMaxCloudTextures - 1) {
            const size_t comma = val.find(',', start);
            const size_t end = (comma == std::string::npos) ? val.size() : comma;
            const std::string name = Trim(val.substr(start, end - start));
            if (!name.empty()) {
                gRequestedTextureNames[gRequestedTextureNameCount++] = name;
            }
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
    }

    static void WriteDefaultIni(const char* path) {
        std::ofstream out(path);
        if (!out.is_open())
            return;
        out << "; VolumetricCloudsExtended settings\n";
        out << "TargetProcClouds=" << gCfg.targetProcClouds << "\n";
        out << "BadWeatherDensityMultiplier=" << gCfg.badWeatherDensityMultiplier << "\n";
        out << "MinSpawnZ=" << gCfg.minSpawnZ << "\n";
        out << "SpawnRadiusMin=" << gCfg.spawnRadiusMin << "\n";
        out << "SpawnRadiusMax=" << gCfg.spawnRadiusMax << "\n";
        out << "RenderMaxDist=" << gCfg.renderMaxDist << "\n";
        out << "RespawnMaxDist=" << gCfg.respawnMaxDist << "\n";
        out << "WindMoveScale=" << gCfg.windMoveScale << "\n";
        out << "MinSpacing2D=" << gCfg.minSpacing2D << "\n";
        out << "SpawnFadeInSpeed=" << gCfg.spawnFadeInSpeed << "\n";
        out << "QuadFacingMin=" << gCfg.quadFacingMin << "\n";
        out << "FarClipFadeRange=" << gCfg.farClipFadeRange << "\n";
        out << "FarClipFadeMargin=" << gCfg.farClipFadeMargin << "\n";
        out << "TimecycFarClipScale=" << gCfg.timecycFarClipScale << "\n";
        out << "UseTimecycTint=" << (gCfg.useTimecycTint ? 1 : 0) << "\n";
        out << "TimecycTintStrength=" << gCfg.timecycTintStrength << "\n";
        out << "FluffyCloudBottomBlend=" << gCfg.fluffyCloudBottomBlend << "\n";
        out << "ColorBrightnessBoost=" << gCfg.colorBrightnessBoost << "\n";
        out << "UseAdditionalTextures=" << (gCfg.useAdditionalTextures ? 1 : 0) << "\n";
        out << "IncludeOriginalTextureInRandom=" << (gCfg.includeOriginalTextureInRandom ? 1 : 0) << "\n";
        out << "; Comma-separated particle.txd texture names. Missing names are ignored.\n";
        out << "CloudTextureNames=cloud2,cloud3,cloud4\n";
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
            else if (key == "BadWeatherDensityMultiplier") gCfg.badWeatherDensityMultiplier = f;
            else if (key == "MinSpawnZ") gCfg.minSpawnZ = f;
            else if (key == "SpawnRadiusMin") gCfg.spawnRadiusMin = f;
            else if (key == "SpawnRadiusMax") gCfg.spawnRadiusMax = f;
            else if (key == "RenderMaxDist") gCfg.renderMaxDist = f;
            else if (key == "RespawnMaxDist") gCfg.respawnMaxDist = f;
            else if (key == "WindMoveScale") gCfg.windMoveScale = f;
            else if (key == "MinSpacing2D") gCfg.minSpacing2D = f;
            else if (key == "SpawnFadeInSpeed") gCfg.spawnFadeInSpeed = f;
            else if (key == "QuadFacingMin") gCfg.quadFacingMin = f;
            else if (key == "FarClipFadeRange") gCfg.farClipFadeRange = f;
            else if (key == "FarClipFadeMargin") gCfg.farClipFadeMargin = f;
            else if (key == "TimecycFarClipScale") gCfg.timecycFarClipScale = f;
            else if (key == "UseTimecycTint") gCfg.useTimecycTint = ParseBool(val);
            else if (key == "TimecycTintStrength" || key == "TimecycleTintStrength") gCfg.timecycTintStrength = f;
            else if (key == "FluffyCloudBottomBlend" || key == "TimecycTintSkyInfluence") gCfg.fluffyCloudBottomBlend = f;
            else if (key == "ColorBrightnessBoost") gCfg.colorBrightnessBoost = (int)f;
            else if (key == "UseAdditionalTextures") gCfg.useAdditionalTextures = ParseBool(val);
            else if (key == "IncludeOriginalTextureInRandom") gCfg.includeOriginalTextureInRandom = ParseBool(val);
            else if (key == "CloudTextureNames") ParseTextureNameList(val);
        }

        if (gCfg.targetProcClouds < 1) gCfg.targetProcClouds = 1;
        if (gCfg.targetProcClouds > kMaxProcClouds) gCfg.targetProcClouds = kMaxProcClouds;
        if (gCfg.badWeatherDensityMultiplier < 1.0f) gCfg.badWeatherDensityMultiplier = 1.0f;
        if (gCfg.badWeatherDensityMultiplier > 3.0f) gCfg.badWeatherDensityMultiplier = 3.0f;
        if (gCfg.spawnRadiusMin < 1.0f) gCfg.spawnRadiusMin = 1.0f;
        if (gCfg.spawnRadiusMax < gCfg.spawnRadiusMin + 1.0f) gCfg.spawnRadiusMax = gCfg.spawnRadiusMin + 1.0f;
        if (gCfg.respawnMaxDist < gCfg.renderMaxDist + 50.0f) gCfg.respawnMaxDist = gCfg.renderMaxDist + 50.0f;
        if (gCfg.minSpacing2D < 1.0f) gCfg.minSpacing2D = 1.0f;
        if (gCfg.spawnFadeInSpeed < 0.001f) gCfg.spawnFadeInSpeed = 0.001f;
        if (gCfg.quadFacingMin < 0.0f) gCfg.quadFacingMin = 0.0f;
        if (gCfg.quadFacingMin > 1.0f) gCfg.quadFacingMin = 1.0f;
        if (gCfg.farClipFadeRange < 50.0f) gCfg.farClipFadeRange = 50.0f;
        if (gCfg.farClipFadeMargin < 0.0f) gCfg.farClipFadeMargin = 0.0f;
        if (gCfg.farClipFadeMargin > 500.0f) gCfg.farClipFadeMargin = 500.0f;
        if (gCfg.timecycFarClipScale < 1.0f) gCfg.timecycFarClipScale = 1.0f;
        if (gCfg.timecycFarClipScale > 4.0f) gCfg.timecycFarClipScale = 4.0f;
        if (gCfg.timecycTintStrength < 0.0f) gCfg.timecycTintStrength = 0.0f;
        if (gCfg.timecycTintStrength > 1.0f) gCfg.timecycTintStrength = 1.0f;
        if (gCfg.fluffyCloudBottomBlend < 0.0f) gCfg.fluffyCloudBottomBlend = 0.0f;
        if (gCfg.fluffyCloudBottomBlend > 1.0f) gCfg.fluffyCloudBottomBlend = 1.0f;
        if (gCfg.colorBrightnessBoost < 0) gCfg.colorBrightnessBoost = 0;
        if (gCfg.colorBrightnessBoost > 255) gCfg.colorBrightnessBoost = 255;
    }

    static unsigned char ClampU8(int v) {
        if (v < 0)
            return 0;
        if (v > 255)
            return 255;
        return (unsigned char)v;
    }

    static unsigned char LerpU8(int a, int b, float t) {
        return ClampU8((int)((float)a + ((float)b - (float)a) * t + 0.5f));
    }


    static float GetCloudFadeReferenceDistance() {
        const auto& cc = CTimeCycle::m_CurrentColours;

        // GTA SA 1.0 US CDraw::ms_fLODDistance and CDraw::ms_fFarClipZ.
        // D3D9 fog/far clip can be shorter than the useful sky/cloud draw range, so prefer
        // the larger LOD/timecycle-derived distance and only use draw far clip as one input.
        const float lodDistance = *reinterpret_cast<float*>(0xC3EF98);
        const float drawFarClip = *reinterpret_cast<float*>(0xC3EF9C);
        const float timecycFarClip = cc.m_fFarClip * gCfg.timecycFarClipScale;

        float referenceDist = gCfg.renderMaxDist;
        if (lodDistance > 100.0f)
            referenceDist = std::max(referenceDist, lodDistance);
        if (timecycFarClip > 100.0f)
            referenceDist = std::max(referenceDist, timecycFarClip);
        if (drawFarClip > 100.0f)
            referenceDist = std::max(referenceDist, drawFarClip);

        return std::max(300.0f, std::min(referenceDist, gCfg.respawnMaxDist));
    }

    static void GetDistanceFadeWindow(float& fadeBegin, float& fadeEnd) {
        const auto& cc = CTimeCycle::m_CurrentColours;
        const float referenceDist = GetCloudFadeReferenceDistance();
        const float safeFarEnd = std::max(300.0f, referenceDist - gCfg.farClipFadeMargin);
        const float cfgEnd = std::min(gCfg.respawnMaxDist, safeFarEnd);
        const float cfgBegin = std::min(gCfg.renderMaxDist - 120.0f, cfgEnd - gCfg.farClipFadeRange);

        fadeEnd = cfgEnd;
        fadeBegin = cfgBegin;

        // If the active timecycle fog starts before our normal fade, start fading there.
        // This prevents D3D9 linear fog from making distant procedural clouds look hard-clipped.
        if (cc.m_fFogStart > 1.0f && cc.m_fFogStart < fadeEnd)
            fadeBegin = std::min(fadeBegin, cc.m_fFogStart);

        if (fadeBegin > fadeEnd - 50.0f)
            fadeBegin = fadeEnd - 50.0f;
        if (fadeBegin < 50.0f)
            fadeBegin = 50.0f;
    }

    static CloudRenderColour GetTimecycCloudColour() {
        const auto& cc = CTimeCycle::m_CurrentColours;

        const unsigned char lowR = ClampU8((int)cc.m_nLowCloudsRed);
        const unsigned char lowG = ClampU8((int)cc.m_nLowCloudsGreen);
        const unsigned char lowB = ClampU8((int)cc.m_nLowCloudsBlue);

        const unsigned char bottomR = ClampU8((int)cc.m_nFluffyCloudsBottomRed);
        const unsigned char bottomG = ClampU8((int)cc.m_nFluffyCloudsBottomGreen);
        const unsigned char bottomB = ClampU8((int)cc.m_nFluffyCloudsBottomBlue);

        const unsigned char timecycR = LerpU8(lowR, bottomR, gCfg.fluffyCloudBottomBlend);
        const unsigned char timecycG = LerpU8(lowG, bottomG, gCfg.fluffyCloudBottomBlend);
        const unsigned char timecycB = LerpU8(lowB, bottomB, gCfg.fluffyCloudBottomBlend);

        // Keep the old vanilla-like grayscale as the 0-strength fallback, but do not
        // luminance-normalize the timecycle RGB; direct low/fluffy cloud RGB is what
        // visibly follows timecyc.dat cloud colour changes in-game.
        const int skyGrey = std::min(
            ((int)cc.m_nSkyTopRed + (int)cc.m_nSkyTopGreen + (int)cc.m_nSkyTopBlue +
                (int)cc.m_nSkyBottomRed + (int)cc.m_nSkyBottomGreen + (int)cc.m_nSkyBottomBlue) / 6 + gCfg.colorBrightnessBoost,
            255
        );

        if (!gCfg.useTimecycTint || gCfg.timecycTintStrength <= 0.0f) {
            const unsigned char gray = ClampU8(skyGrey);
            return { gray, gray, gray };
        }

        return {
            LerpU8(skyGrey, timecycR, gCfg.timecycTintStrength),
            LerpU8(skyGrey, timecycG, gCfg.timecycTintStrength),
            LerpU8(skyGrey, timecycB, gCfg.timecycTintStrength)
        };
    }

    static void ShutdownCloudTextures() {
        for (int i = 0; i < gCloudTextureCount; i++) {
            if (gCloudTextures[i].ownedByPlugin && gCloudTextures[i].texture) {
                DestroyRwTexture(gCloudTextures[i].texture);
            }
            gCloudTextures[i].texture = nullptr;
            gCloudTextures[i].ownedByPlugin = false;
        }
        gCloudTextureCount = 0;
        gCloudTexturesInitialized = false;
        gOriginalTextureAtLastLoad = nullptr;
    }

    static void AddCloudTexture(RwTexture* texture, bool ownedByPlugin) {
        if (!texture)
            return;
        for (int i = 0; i < gCloudTextureCount; i++) {
            if (gCloudTextures[i].texture == texture)
                return;
        }
        if (gCloudTextureCount >= kMaxCloudTextures) {
            if (ownedByPlugin)
                DestroyRwTexture(texture);
            return;
        }
        gCloudTextures[gCloudTextureCount].texture = texture;
        gCloudTextures[gCloudTextureCount].ownedByPlugin = ownedByPlugin;
        gCloudTextureCount++;
    }

    static void EnsureCloudTexturesLoaded() {
        RwTexture* originalTexture = CClouds::ms_vc.m_pTex;
        if (gCloudTexturesInitialized && gCloudTextureCount > 0 && gOriginalTextureAtLastLoad == originalTexture)
            return;

        ShutdownCloudTextures();
        gOriginalTextureAtLastLoad = originalTexture;
        AddCloudTexture(originalTexture, false);

        if (gCfg.useAdditionalTextures) {
            const int particleSlot = CTxdStore::FindTxdSlot("particle");
            if (particleSlot >= 0) {
                CTxdStore::PushCurrentTxd();
                CTxdStore::SetCurrentTxd(particleSlot);
                for (int i = 0; i < gRequestedTextureNameCount && gCloudTextureCount < kMaxCloudTextures; i++) {
                    RwTexture* tex = ReadRwTexture(gRequestedTextureNames[i].c_str(), nullptr);
                    AddCloudTexture(tex, tex != nullptr);
                }
                CTxdStore::PopCurrentTxd();
            }
        }

        if (!gCfg.includeOriginalTextureInRandom && originalTexture && gCloudTextureCount > 1 && gCloudTextures[0].texture == originalTexture) {
            for (int i = 1; i < gCloudTextureCount; i++) {
                gCloudTextures[i - 1] = gCloudTextures[i];
            }
            gCloudTextureCount--;
            gCloudTextures[gCloudTextureCount].texture = nullptr;
            gCloudTextures[gCloudTextureCount].ownedByPlugin = false;
        }
        if (gCloudTextureCount == 0) {
            AddCloudTexture(originalTexture, false);
        }
        gCloudTexturesInitialized = true;
    }

    static unsigned char PickRandomTextureIndex() {
        EnsureCloudTexturesLoaded();
        if (gCloudTextureCount <= 1)
            return 0;
        return (unsigned char)CGeneral::GetRandomNumberInRange(0, gCloudTextureCount);
    }

    static RwTexture* GetCloudTextureForIndex(unsigned char index) {
        EnsureCloudTexturesLoaded();
        if (gCloudTextureCount <= 0)
            return CClouds::ms_vc.m_pTex;
        if (index >= gCloudTextureCount)
            return gCloudTextures[0].texture;
        return gCloudTextures[index].texture ? gCloudTextures[index].texture : gCloudTextures[0].texture;
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


    static float Clamp01(float v) {
        if (v < 0.0f)
            return 0.0f;
        if (v > 1.0f)
            return 1.0f;
        return v;
    }

    static bool IsCloudyOrRainyWeather(short weatherType) {
        switch (weatherType) {
        case WEATHER_CLOUDY_LA:
        case WEATHER_CLOUDY_SF:
        case WEATHER_RAINY_SF:
        case WEATHER_CLOUDY_VEGAS:
        case WEATHER_CLOUDY_COUNTRYSIDE:
        case WEATHER_RAINY_COUNTRYSIDE:
            return true;
        default:
            return false;
        }
    }

    static int GetTargetProcClouds() {
        const float interp = Clamp01(CWeather::InterpolationValue);
        const float oldWeatherWeight = IsCloudyOrRainyWeather(CWeather::OldWeatherType) ? 1.0f - interp : 0.0f;
        const float newWeatherWeight = IsCloudyOrRainyWeather(CWeather::NewWeatherType) ? interp : 0.0f;
        float badWeatherFactor = std::max(oldWeatherWeight, newWeatherWeight);

        // Rain is already interpolated by the game, so use it to smoothly increase density
        // for scripted rain and rainy transitions even when weather IDs lag behind.
        badWeatherFactor = std::max(badWeatherFactor, Clamp01(CWeather::Rain));

        const float scaledTarget = (float)gCfg.targetProcClouds * (1.0f + (gCfg.badWeatherDensityMultiplier - 1.0f) * badWeatherFactor);
        int target = (int)(scaledTarget + 0.5f);
        if (target < 1)
            target = 1;
        if (target > kMaxProcClouds)
            target = kMaxProcClouds;
        return target;
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
        c.textureIndex = PickRandomTextureIndex();
    }

    static void ResetCloudPool() {
        for (int i = 0; i < kMaxProcClouds; i++) {
            gProcClouds[i].used = false;
            gProcClouds[i].alpha = 0;
            gProcClouds[i].size = CVector(0.0f, 0.0f, 0.0f);
            gProcClouds[i].fadeIn = 0.0f;
            gProcClouds[i].textureIndex = 0;
        }
        gCloudPoolInitialized = false;
    }

    static void EnsureCloudPoolInitialized() {
        if (gCloudPoolInitialized)
            return;

        // Make sure GTA SA's original volumetric cloud model-space tables are initialized.
        CClouds::VolumetricCloudsInit();
        EnsureCloudTexturesLoaded();

        const CVector camPos = TheCamera.GetPosition();
        gCloudWorldCenter = camPos;

        for (int i = 0; i < kMaxProcClouds; i++) {
            gProcClouds[i].used = false;
            gProcClouds[i].pos = camPos;
            gProcClouds[i].size = CVector(0.0f, 0.0f, 0.0f);
            gProcClouds[i].alpha = 0;
            gProcClouds[i].fadeIn = 0.0f;
            gProcClouds[i].textureIndex = 0;
        }

        const int targetClouds = GetTargetProcClouds();
        for (int i = 0; i < targetClouds; i++) {
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
        const int targetClouds = GetTargetProcClouds();
        if (!gWasOutsideLastFrame) {
            gCloudWorldCenter = camPos;
            for (int i = 0; i < targetClouds; i++) {
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

        for (int i = 0; i < kMaxProcClouds && activeCount < targetClouds; i++) {
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
        *reinterpret_cast<float*>(0xC6E970) = 0.0f; // GTA SA 1.0 US volumetric cloud height fader.

        if (!CGame::CanSeeOutSideFromCurrArea())
            return;

        EnsureCloudTexturesLoaded();
        RwTexture* fallbackCloudTex = CClouds::ms_vc.m_pTex;
        if (!fallbackCloudTex && gCloudTextureCount > 0)
            fallbackCloudTex = gCloudTextures[0].texture;
        if (!fallbackCloudTex) {
            CClouds::VolumetricCloudsRender();
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
        RwRenderStateSet(rwRENDERSTATEFOGENABLE, oldFog);
        RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)rwCULLMODECULLNONE);
        RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*)rwFILTERLINEAR);

        const CVector camPos = TheCamera.GetPosition();
        float fadeOutBeginDist = 0.0f;
        float fadeOutEndDist = 0.0f;
        GetDistanceFadeWindow(fadeOutBeginDist, fadeOutEndDist);
        const CloudRenderColour cloudColour = GetTimecycCloudColour();

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
            const float farEdgeDist = dist + cloudExtent * 0.65f;
            int cloudAlpha = (int)c.alpha;
            if (farEdgeDist > fadeOutBeginDist) {
                if (farEdgeDist > fadeOutEndDist)
                    continue;
                const float denom = std::max(1.0f, fadeOutEndDist - fadeOutBeginDist);
                const float t = (fadeOutEndDist - farEdgeDist) / denom;
                const float smoothT = std::max(0.0f, std::min(1.0f, t));
                const int distAlpha = (int)(smoothT * smoothT * cloudAlpha);
                cloudAlpha = std::min(cloudAlpha, distAlpha);
            }
            const float spawnFade = c.fadeIn * c.fadeIn;
            cloudAlpha = (int)(cloudAlpha * spawnFade);

            const unsigned char alpha = (unsigned char)cloudAlpha;
            if (alpha < 6)
                continue;

            RwTexture* cloudTex = GetCloudTextureForIndex(c.textureIndex);
            if (!cloudTex)
                cloudTex = fallbackCloudTex;
            RwRenderStateSet(rwRENDERSTATETEXTURERASTER, RwTextureGetRaster(cloudTex));

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
                RwIm3DVertexSetRGBA(&v[k], cloudColour.r, cloudColour.g, cloudColour.b, quadAlpha);
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
            patch::RedirectCall(0x53E1B4, RenderProceduralClouds); // GTA SA 1.0 US: CClouds::Render call to volumetric cloud renderer.
            Events::gameProcessEvent += UpdateProceduralClouds;
            Events::shutdownRwEvent += ShutdownCloudTextures;
        }
    };
}

VolumetricCloudsExtended gVolumetricCloudsExtended;
