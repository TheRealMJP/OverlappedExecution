//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#include "PCH.h"
#include "DX12.h"
#include "DX12_Upload.h"
#include "DX12_Helpers.h"
#include "GraphicsTypes.h"

#if Debug_
    #define UseDebugDevice_ 1
    #define BreakOnDXError_ (UseDebugDevice_ && 1)
#else
    #define UseDebugDevice_ 0
    #define BreakOnDXError_ 0
#endif

namespace SampleFramework12
{

namespace DX12
{

ID3D12Device* Device = nullptr;
ID3D12GraphicsCommandList* CmdList = nullptr;
ID3D12CommandQueue* GfxQueue = nullptr;
D3D_FEATURE_LEVEL FeatureLevel = D3D_FEATURE_LEVEL_11_0;
IDXGIFactory4* Factory = nullptr;
IDXGIAdapter1* Adapter = nullptr;

uint64 CurrentCPUFrame = 0;
uint64 CurrentGPUFrame = 0;
uint64 CurrFrameIdx = 0;

static const uint64 NumCmdAllocators = RenderLatency;

static ID3D12CommandAllocator* CmdAllocators[NumCmdAllocators] = { };
static Fence FrameFence;

static GrowableList<IUnknown*> DeferredReleases[RenderLatency];
static bool ShuttingDown = false;

static void ProcessDeferredReleases(uint64 idx)
{
    for(uint64 i = 0; i < DeferredReleases[idx].Count(); ++i)
        DeferredReleases[idx][i]->Release();
    DeferredReleases[idx].RemoveAll(nullptr);
}

void Initialize(D3D_FEATURE_LEVEL minFeatureLevel, uint32 adapterIdx)
{
    ShuttingDown = false;

    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&Factory));
    if(FAILED(hr))
        throw Exception(L"Unable to create a DXGI 1.4 device.\n "
                        L"Make sure that your OS and driver support DirectX 12");

    LARGE_INTEGER umdVersion = { };
    Factory->EnumAdapters1(adapterIdx, &Adapter);

    if(Adapter == nullptr)
        throw Exception(L"Unable to locate a DXGI 1.4 adapter that supports a D3D12 device.\n"
                        L"Make sure that your OS and driver support DirectX 12");

    DXGI_ADAPTER_DESC1 desc = { };
    Adapter->GetDesc1(&desc);
    WriteLog("Creating DX12 device on adapter '%ls'", desc.Description);

    #if UseDebugDevice_
        ID3D12DebugPtr d3d12debug;
        DXCall(D3D12GetDebugInterface(IID_PPV_ARGS(&d3d12debug)));
        d3d12debug->EnableDebugLayer();
    #endif

    DXCall(D3D12CreateDevice(Adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device)));

    // Check the maximum feature level, and make sure it's above our minimum
    D3D_FEATURE_LEVEL featureLevelsArray[4];
    featureLevelsArray[0] = D3D_FEATURE_LEVEL_11_0;
    featureLevelsArray[1] = D3D_FEATURE_LEVEL_11_1;
    featureLevelsArray[2] = D3D_FEATURE_LEVEL_12_0;
    featureLevelsArray[3] = D3D_FEATURE_LEVEL_12_1;
    D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = { };
    featureLevels.NumFeatureLevels = ArraySize_(featureLevelsArray);
    featureLevels.pFeatureLevelsRequested = featureLevelsArray;
    DXCall(Device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels, sizeof(featureLevels)));
    FeatureLevel = featureLevels.MaxSupportedFeatureLevel;

    if(FeatureLevel < minFeatureLevel)
    {
        std::wstring majorLevel = ToString<int>(minFeatureLevel >> 12);
        std::wstring minorLevel = ToString<int>((minFeatureLevel >> 8) & 0xF);
        throw Exception(L"The device doesn't support the minimum feature level required to run this sample (DX" + majorLevel + L"." + minorLevel + L")");
    }

    #if UseDebugDevice_
        ID3D12InfoQueuePtr infoQueue;
        DXCall(Device->QueryInterface(IID_PPV_ARGS(&infoQueue)));

        D3D12_MESSAGE_ID disabledMessages[] =
        {
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,

            // These happen when capturing with VS diagnostics
            D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
            D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,

            // There's a bug in the validation layer that assumes that all desriptor handles are globally unique
            D3D12_MESSAGE_ID_COPY_DESCRIPTORS_INVALID_RANGES,
        };

        D3D12_INFO_QUEUE_FILTER filter = { };
        filter.DenyList.NumIDs = ArraySize_(disabledMessages);
        filter.DenyList.pIDList = disabledMessages;
        infoQueue->AddStorageFilterEntries(&filter);
    #endif

    #if BreakOnDXError_
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
    #endif

    for(uint64 i = 0; i < NumCmdAllocators; ++i)
    {
        DXCall(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&CmdAllocators[i])));
        CmdAllocators[i]->SetName(L"Gfx Command Allocator");
    }


    DXCall(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, CmdAllocators[0], nullptr, IID_PPV_ARGS(&CmdList)));
    CmdList->SetName(L"Gfx Command List");
    DXCall(CmdList->Close());

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    DXCall(Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&GfxQueue)));
    GfxQueue->SetName(L"Gfx Command Queue");

    CurrFrameIdx = CurrentCPUFrame % NumCmdAllocators;
    DXCall(CmdAllocators[CurrFrameIdx]->Reset());
    DXCall(CmdList->Reset(CmdAllocators[CurrFrameIdx], nullptr));

    FrameFence.Init(0);

    Initialize_Upload();
    Initialize_Helpers();
}

void Shutdown()
{
    Assert_(CurrentCPUFrame == CurrentGPUFrame);
    ShuttingDown = true;

    for(uint64 i = 0; i < ArraySize_(DeferredReleases); ++i)
        ProcessDeferredReleases(i);

    FrameFence.Shutdown();

    for(uint64 i = 0; i < RenderLatency; ++i)
        Release(CmdAllocators[i]);

    Release(CmdList);
    Release(GfxQueue);
    Release(Factory);
    Release(Adapter);

    Shutdown_Upload();
    Shutdown_Helpers();

    #if BreakOnDXError_
        if(Device != nullptr)
        {
            ID3D12InfoQueuePtr infoQueue;
            DXCall(Device->QueryInterface(IID_PPV_ARGS(&infoQueue)));
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        }
    #endif

    #if UseDebugDevice_ && 0
        if(Device != nullptr)
        {
            ID3D12DebugDevicePtr debugDevice;
            DXCall(Device->QueryInterface(IID_PPV_ARGS(&debugDevice)));
            debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL);
        }
    #endif

    Release(Device);
}

void BeginFrame()
{
    Assert_(Device);

    SetDescriptorHeaps(CmdList);
}

void EndFrame(IDXGISwapChain4* swapChain, uint32 syncIntervals)
{
    Assert_(Device);

    DXCall(CmdList->Close());

    EndFrame_Upload();

    ID3D12CommandList* commandLists[] = { CmdList };
    GfxQueue->ExecuteCommandLists(ArraySize_(commandLists), commandLists);

    // Present the frame.
    if(swapChain)
        DXCall(swapChain->Present(syncIntervals, syncIntervals == 0 ? DXGI_PRESENT_ALLOW_TEARING : 0));

    ++CurrentCPUFrame;

    // Signal the fence with the current frame number, so that we can check back on it
    FrameFence.Signal(GfxQueue, CurrentCPUFrame);

    // Wait for the GPU to catch up before we stomp an executing command buffer
    const uint64 gpuLag = DX12::CurrentCPUFrame - DX12::CurrentGPUFrame;
    Assert_(gpuLag <= DX12::RenderLatency);
    if(gpuLag >= DX12::RenderLatency)
    {
        // Make sure that the previous frame is finished
        FrameFence.Wait(DX12::CurrentGPUFrame + 1);
        ++DX12::CurrentGPUFrame;
    }

    CurrFrameIdx = DX12::CurrentCPUFrame % NumCmdAllocators;

    // Prepare the command buffers to be used for the next frame
    DXCall(CmdAllocators[CurrFrameIdx]->Reset());
    DXCall(CmdList->Reset(CmdAllocators[CurrFrameIdx], nullptr));

    EndFrame_Helpers();

    // See if we have any deferred releases to process
    ProcessDeferredReleases(CurrFrameIdx);
}

void FlushGPU()
{
    Assert_(Device);

    // Wait for the GPU to fully catch up with the CPU
    Assert_(CurrentCPUFrame >= CurrentGPUFrame);
    if(CurrentCPUFrame > CurrentGPUFrame)
    {
        FrameFence.Wait(CurrentCPUFrame);
        CurrentGPUFrame = CurrentCPUFrame;
    }

    // Clean up what we can now
    for(uint64 i = 1; i < RenderLatency; ++i)
        ProcessDeferredReleases((i + CurrFrameIdx) % RenderLatency);
}

void DeferredRelease_(IUnknown* resource)
{
    if(resource == nullptr)
        return;

    if(ShuttingDown || Device == nullptr)
    {
        // Free-for-all!
        resource->Release();
        return;
    }

    DeferredReleases[CurrFrameIdx].Add(resource);
}

} // namespace SampleFramework12

} // namespace SampleFramework12

