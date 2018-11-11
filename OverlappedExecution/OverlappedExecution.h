//=================================================================================================
//
//  Overlapped Execution Sample
//  by MJP
//  http://mynameismjp.wordpress.com/
//
//  All code and content licensed under the MIT license
//
//=================================================================================================

#pragma once

#include <PCH.h>

#include <App.h>
#include <Graphics/GraphicsTypes.h>

using namespace SampleFramework12;

enum Workloads : uint64
{
    ComputeWorkloadA = 0,
    ComputeWorkloadB,
    ComputeWorkloadC,
    GfxWorkloadA,
    ComputeWorkloadD,
    ComputeQueueWorkloadA,
    ComputeQueueWorkloadB,
    ComputeQueueWorkloadC,

    NumWorkloads,
    NumGfxQueueWorkloads = ComputeQueueWorkloadA,
    NumComputeQueueWorkloads = NumWorkloads - ComputeQueueWorkloadA,
};

enum class WorkloadType : uint64
{
    Compute,
    Graphics,
    ComputeQueue,
};

struct Workload
{
    ID3D12Heap* ReadbackHeap = nullptr;
    StructuredBuffer ShaderStartBuffer;
    StructuredBuffer ShaderEndBuffer;
    uint32* ShaderStartData = nullptr;
    uint32* ShaderEndData = nullptr;
    RawBuffer CounterBuffer;
    float StartTimes[64] = { };
    float EndTimes[64] = { };
    float StartTime = 0;
    float EndTime = 0;
    const char* Name = nullptr;
    uint64 LastUpdatedFrame = uint64(-1);
    WorkloadType Type = WorkloadType::Compute;
    int32 NumGroups = 8;
    int32 NumIterations = 64;
    bool Enabled = true;
    bool BufferIsReadable = false;
    uint64 DependsOn = uint64(-1);
};

class OverlappedExecution : public App
{

protected:

    CompiledShaderPtr workloadCS;
    ID3D12PipelineState* workloadCSPSO = nullptr;

    CompiledShaderPtr workloadVS;
    CompiledShaderPtr workloadPS;
    ID3D12PipelineState* workloadGfxPSO = nullptr;
    RenderTexture workloadRT;

    ID3D12RootSignature* workloadRootSignature = nullptr;
    StructuredBuffer workloadInputBuffer;
    StructuredBuffer workloadOutputBuffer;
    StructuredBuffer computeWorkloadOutputBuffer;

    Workload workloads[NumWorkloads];

    Fence waitFence;

    ID3D12GraphicsCommandList* computeCmdList = nullptr;
    ID3D12CommandQueue* computeQueue = nullptr;
    ID3D12CommandQueue* hiPriorityComputeQueue = nullptr;

    ID3D12CommandAllocator* cmdAllocators[DX12::RenderLatency] = { };
    Fence computeFence;
    uint64 currentComputeFrame = 0;

    DXGI_ADAPTER_DESC1 adapterDesc = { };

    struct WorkloadConstants
    {
        uint32 FrameNum;
        uint32 NumIterations;
        uint32 NumWorkloadElements;
    };

    ConstantBuffer<WorkloadConstants> workloadCBuffer;

    virtual void Initialize() override;
    virtual void Shutdown() override;

    virtual void Render(const Timer& timer) override;
    virtual void Update(const Timer& timer) override;

    virtual void BeforeReset() override;
    virtual void AfterReset() override;

    virtual void CreatePSOs() override;
    virtual void DestroyPSOs() override;

    virtual void BeforeFlush() override;

    void RenderCompute();
    void DoComputeWorkload(ID3D12GraphicsCommandList* cmdList, Workload& workload, const StructuredBuffer& workloadOutput);
    void DoGraphicsWorkload(Workload& workload);
    void RenderHUD();
    void RenderWorkloadUI();

public:

    OverlappedExecution(const wchar* cmdLine);
};
