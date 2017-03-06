#pragma once

#include <PCH.h>
#include <Settings.h>
#include <Graphics\GraphicsTypes.h>

using namespace SampleFramework12;

namespace AppSettings
{
    static const int64 MaxWorkloadElements = 262144;
    static const int64 MaxWorkloadIterations = 128;
    static const int64 WorkloadGroupSize = 1024;
    static const int64 WorkloadRTWidth = 1024;
    static const int64 MaxWorkloadGroups = 256;

    extern BoolSetting EnableVSync;
    extern FloatSetting TimelineZoom;
    extern BoolSetting UseSplitBarriers;
    extern BoolSetting StablePowerState;
    extern BoolSetting UseHiPriorityComputeQueue;
    extern BoolSetting ShowWorkloadUI;

    struct AppSettingsCBuffer
    {
        uint32 Dummy;
    };

    extern ConstantBuffer<AppSettingsCBuffer> CBuffer;
    const extern uint32 CBufferRegister;

    void Initialize();
    void Shutdown();
    void Update(uint32 displayWidth, uint32 displayHeight, const Float4x4& viewMatrix);
    void UpdateCBuffer();
    void BindCBufferGfx(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter);
    void BindCBufferCompute(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter);
};

// ================================================================================================

namespace AppSettings
{
    void SetWindowOpened(bool opened);
}