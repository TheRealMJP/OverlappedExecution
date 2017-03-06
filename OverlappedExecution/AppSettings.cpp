#include <PCH.h>
#include "AppSettings.h"

using namespace SampleFramework12;

namespace AppSettings
{
    static SettingsContainer Settings;

    BoolSetting EnableVSync;
    FloatSetting TimelineZoom;
    BoolSetting UseSplitBarriers;
    BoolSetting StablePowerState;
    BoolSetting UseHiPriorityComputeQueue;
    BoolSetting ShowWorkloadUI;

    ConstantBuffer<AppSettingsCBuffer> CBuffer;
    const uint32 CBufferRegister = 12;

    void Initialize()
    {

        Settings.Initialize(1);

        Settings.AddGroup("General", true);

        EnableVSync.Initialize("EnableVSync", "General", "Enable VSync", "Enables or disables vertical sync during Present", true);
        Settings.AddSetting(&EnableVSync);

        TimelineZoom.Initialize("TimelineZoom", "General", "Timeline Zoom", "", 1.0000f, 1.0000f, 16.0000f, 0.0100f, ConversionMode::None, 1.0000f);
        Settings.AddSetting(&TimelineZoom);

        UseSplitBarriers.Initialize("UseSplitBarriers", "General", "Use Split Barriers", "", false);
        Settings.AddSetting(&UseSplitBarriers);

        StablePowerState.Initialize("StablePowerState", "General", "Stable Power State", "", false);
        Settings.AddSetting(&StablePowerState);

        UseHiPriorityComputeQueue.Initialize("UseHiPriorityComputeQueue", "General", "Use Hi Priority Compute Queue", "", false);
        Settings.AddSetting(&UseHiPriorityComputeQueue);

        ShowWorkloadUI.Initialize("ShowWorkloadUI", "General", "Show Workload UI", "", true);
        Settings.AddSetting(&ShowWorkloadUI);

        CBuffer.Initialize(BufferLifetime::Temporary);
    }

    void Update(uint32 displayWidth, uint32 displayHeight, const Float4x4& viewMatrix)
    {
        Settings.Update(displayWidth, displayHeight, viewMatrix);

    }

    void UpdateCBuffer()
    {

        CBuffer.Upload();
    }
    void BindCBufferGfx(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter)
    {
        CBuffer.SetAsGfxRootParameter(cmdList, rootParameter);
    }
    void BindCBufferCompute(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter)
    {
        CBuffer.SetAsComputeRootParameter(cmdList, rootParameter);
    }
    void Shutdown()
    {
        CBuffer.Shutdown();
    }
}

// ================================================================================================

namespace AppSettings
{
    void SetWindowOpened(bool opened)
    {
        Settings.SetWindowOpened(opened);
    }
}