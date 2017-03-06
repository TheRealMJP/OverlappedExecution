//=================================================================================================
//
//  Overlapped Execution Sample
//  by MJP
//  http://mynameismjp.wordpress.com/
//
//  All code and content licensed under the MIT license
//
//=================================================================================================

#include <PCH.h>

#include <Input.h>
#include <Utility.h>
#include <ImGuiHelper.h>
#include <Graphics/ShaderCompilation.h>
#include <Graphics/Profiler.h>
#include <Graphics/DX12.h>
#include <Graphics/DX12_Helpers.h>

#include "OverlappedExecution.h"
#include "AppSettings.h"

using namespace SampleFramework12;

static const char* WorkloadNames[] =
{
    "Compute Workload A",
    "Compute Workload B",
    "Compute Workload C",
    "Gfx Workload A",
    "Compute Workload D",
    "Compute Queue Workload A",
    "Compute Queue Workload B",
    "Compute Queue Workload C",
};

static const WorkloadType WorkloadTypes[] =
{
    WorkloadType::Compute,
    WorkloadType::Compute,
    WorkloadType::Compute,
    WorkloadType::Graphics,
    WorkloadType::Compute,
    WorkloadType::ComputeQueue,
    WorkloadType::ComputeQueue,
    WorkloadType::ComputeQueue,
};

StaticAssert_(ArraySize_(WorkloadNames) == NumWorkloads);
StaticAssert_(ArraySize_(WorkloadTypes) == NumWorkloads);

// Initializes the backing data for a single compute or graphics workload
static void InitWorkload(Workload& workload, const char* name, WorkloadType type, uint64 workloadIdx)
{
    // Create a custom heap that's uncached for the CPU, and (hopefully) uncached for the GPU as well
    D3D12_HEAP_DESC heapDesc = { };
    heapDesc.SizeInBytes = 128 * 1024;
    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    heapDesc.Alignment = 0;
    heapDesc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
    heapDesc.Properties.CreationNodeMask = 0;
    heapDesc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    heapDesc.Properties.VisibleNodeMask = 0;
    DXCall(DX12::Device->CreateHeap(&heapDesc, IID_PPV_ARGS(&workload.ReadbackHeap)));

    // Create two buffers placed in our special heap: one to track when the workload starts executing,
    // and one to track when it completes.
    StructuredBufferInit readbackInit;
    readbackInit.Stride = sizeof(uint32);
    readbackInit.NumElements = 1;
    readbackInit.CreateUAV = true;
    readbackInit.InitialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    readbackInit.Heap = workload.ReadbackHeap;
    readbackInit.HeapOffset = 0;
    readbackInit.Name = L"Shader Start Buffer";
    workload.ShaderStartBuffer.Initialize(readbackInit);

    readbackInit.HeapOffset = 64 * 1024;
    workload.ShaderEndBuffer.Initialize(readbackInit);

    DXCall(workload.ShaderStartBuffer.Resource()->Map(0, nullptr, reinterpret_cast<void**>(&workload.ShaderStartData)));
    DXCall(workload.ShaderEndBuffer.Resource()->Map(0, nullptr, reinterpret_cast<void**>(&workload.ShaderEndData)));

    // Create a buffer that we can perform atomics to determine the actual order that threads start
    // executing within a single workload
    RawBufferInit counterBufferInit;
    counterBufferInit.NumElements = 1;
    counterBufferInit.CreateUAV = true;
    counterBufferInit.InitialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    workload.CounterBuffer.Initialize(counterBufferInit);

    workload.Name = name;
    workload.Type = type;

    // Default to depending on the previous workload
    if(workloadIdx > 0 && workloadIdx != ComputeQueueWorkloadA)
        workload.DependsOn = workloadIdx - 1;
}

static void ShutdownWorkload(Workload& workload)
{
    workload.ShaderStartBuffer.Shutdown();
    workload.ShaderEndBuffer.Shutdown();
    DX12::Release(workload.ReadbackHeap);
    workload.CounterBuffer.Shutdown();
}

OverlappedExecution::OverlappedExecution(const wchar* cmdLine) :  App(L"Overlapped Execution", cmdLine)
{
    minFeatureLevel = D3D_FEATURE_LEVEL_12_0;
}

void OverlappedExecution::BeforeReset()
{
}

void OverlappedExecution::AfterReset()
{
}

void OverlappedExecution::Initialize()
{
    for(uint64 i = 0; i < NumWorkloads; ++i)
        InitWorkload(workloads[i], WorkloadNames[i], WorkloadTypes[i], i);

    {
        // Create buffer with dummy input values to be read by workload shaders
        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(Float4);
        sbInit.NumElements = AppSettings::MaxWorkloadElements * AppSettings::MaxWorkloadIterations;
        sbInit.Name = L"Dummy Data Buffer";
        workloadInputBuffer.Initialize(sbInit);
    }

    {
        // Create a buffer with dummy output values to be written to by workload shaders
        StructuredBufferInit sbInit;
        sbInit.Stride = sizeof(float);
        sbInit.NumElements = AppSettings::MaxWorkloadElements * AppSettings::MaxWorkloadIterations;
        sbInit.Name = L"Dummy Output Buffer";
        sbInit.CreateUAV = true;
        sbInit.InitialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        workloadOutputBuffer.Initialize(sbInit);
    }

    // Make a render target big enough to have 1 texel for every graphics workload thread
    workloadRT.Initialize(AppSettings::WorkloadRTWidth, AppSettings::MaxWorkloadGroups, DXGI_FORMAT_R16G16B16A16_FLOAT,
                          1, 1, false, D3D12_RESOURCE_STATE_RENDER_TARGET);

    {
        // Create the workload root signature
        D3D12_DESCRIPTOR_RANGE1 descriptorRanges[2] = {};
        descriptorRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        descriptorRanges[0].NumDescriptors = 1;
        descriptorRanges[0].BaseShaderRegister = 0;
        descriptorRanges[0].RegisterSpace = 0;
        descriptorRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        descriptorRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        descriptorRanges[1].NumDescriptors = 4;
        descriptorRanges[1].BaseShaderRegister = 0;
        descriptorRanges[1].RegisterSpace = 0;
        descriptorRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER1 rootParameters[2] = {};

        // Descriptors
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[0].DescriptorTable.pDescriptorRanges = descriptorRanges;
        rootParameters[0].DescriptorTable.NumDescriptorRanges = ArraySize_(descriptorRanges);

        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParameters[1].Descriptor.RegisterSpace = 0;
        rootParameters[1].Descriptor.ShaderRegister = 0;

        D3D12_ROOT_SIGNATURE_DESC1 rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = ArraySize_(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 0;
        rootSignatureDesc.pStaticSamplers = nullptr;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        DX12::CreateRootSignature(&workloadRootSignature, rootSignatureDesc);
    }

    workloadCS = CompileFromFile(L"Workload.hlsl", "WorkloadCS", ShaderType::Compute, ShaderProfile::SM51);
    workloadVS = CompileFromFile(L"Workload.hlsl", "WorkloadVS", ShaderType::Vertex, ShaderProfile::SM51);
    workloadPS = CompileFromFile(L"Workload.hlsl", "WorkloadPS", ShaderType::Pixel, ShaderProfile::SM51);

    waitFence.Init(0);
    computeFence.Init(0);

    workloadCBuffer.Initialize(BufferLifetime::Temporary);

    {
        // Create resources for submitting on a compute queue
        for(uint64 i = 0; i < DX12::RenderLatency; ++i)
            DXCall(DX12::Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&cmdAllocators[i])));

        DXCall(DX12::Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, cmdAllocators[0], nullptr, IID_PPV_ARGS(&computeCmdList)));
        DXCall(computeCmdList->Close());

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        DXCall(DX12::Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&computeQueue)));

        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        DXCall(DX12::Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&hiPriorityComputeQueue)));

        DXCall(cmdAllocators[DX12::CurrFrameIdx]->Reset());
        DXCall(computeCmdList->Reset(cmdAllocators[DX12::CurrFrameIdx], nullptr));
    }

    DX12::Adapter->GetDesc1(&adapterDesc);

    AppSettings::SetWindowOpened(false);
}

// Checks the value of the shader start/end buffers to see when workloads start,
// and when they complete
static void TimeWorkloads(Workload** workloads, uint64 numWorkloads)
{
    uint64 completionCount = 0;

    Timer shaderTimer;
    shaderTimer.Update();
    const uint32 signalValue = uint32(DX12::CurrentCPUFrame - 1);

    for(uint64 i = 0; i < numWorkloads; ++i)
    {
        workloads[i]->StartTime = 0.0f;
        workloads[i]->EndTime = 0.0f;
    }

    // Keep looping until we've seen the completion of all active workloads
    while(completionCount < numWorkloads)
    {
        for(uint64 workloadIdx = 0; workloadIdx < numWorkloads; ++workloadIdx)
        {
            Workload& workload = *workloads[workloadIdx];
            if(workload.StartTime != 0.0f && workload.EndTime != 0.0f)
                continue;

            const uint32* shaderStarted = workload.ShaderStartData;
            const uint32* shaderEnded = workload.ShaderEndData;

            shaderTimer.Update();
            if(workload.StartTime == 0.0f && *shaderStarted >= signalValue)
                workload.StartTime = shaderTimer.ElapsedMillisecondsF();

            shaderTimer.Update();
            if(workload.EndTime == 0.0f && *shaderEnded >= signalValue)
                workload.EndTime = shaderTimer.ElapsedMillisecondsF();

            if(workload.StartTime != 0.0f && workload.EndTime != 0.0f)
                ++completionCount;
        }
    }

    // Filter the workload timings to reduce noise
    for(uint64 workloadIdx = 0; workloadIdx < numWorkloads; ++workloadIdx)
    {
        Workload& workload = *workloads[workloadIdx];

        const uint64 filterSize = ArraySize_(workload.StartTimes);
        const uint64 idx = DX12::CurrentCPUFrame % filterSize;
        workload.StartTimes[idx] = workload.StartTime;
        workload.EndTimes[idx] = workload.EndTime;

        float startTimeSum = 0.0f;
        float endTimeSum = 0.0f;
        for(uint64 i = 0; i < filterSize; ++i)
        {
            startTimeSum += workload.StartTimes[i];
            endTimeSum += workload.EndTimes[i];
        }

        workload.StartTime = float(startTimeSum / filterSize);
        workload.EndTime = float(endTimeSum / filterSize);
    }
}

void OverlappedExecution::Shutdown()
{
    for(uint64 i = 0; i < DX12::RenderLatency; ++i)
        DX12::Release(cmdAllocators[i]);
    DX12::Release(computeCmdList);
    DX12::Release(computeQueue);
    DX12::Release(hiPriorityComputeQueue);
    for(uint64 i = 0; i < NumWorkloads; ++i)
        ShutdownWorkload(workloads[i]);
    DX12::Release(workloadRootSignature);
    waitFence.Shutdown();
    computeFence.Shutdown();
    workloadCBuffer.Shutdown();
    workloadInputBuffer.Shutdown();
    workloadOutputBuffer.Shutdown();
    workloadRT.Shutdown();
}

void OverlappedExecution::CreatePSOs()
{
    {
        // Compute workload PSO
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = { };
        psoDesc.CS = workloadCS.ByteCode();
        psoDesc.pRootSignature = workloadRootSignature;
        DXCall(DX12::Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&workloadCSPSO)));
    }

    {
        // Graphics workload PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = { };
        psoDesc.pRootSignature = workloadRootSignature;
        psoDesc.VS = workloadVS.ByteCode();
        psoDesc.PS = workloadPS.ByteCode();
        psoDesc.RasterizerState = DX12::GetRasterizerState(RasterizerState::NoCull);
        psoDesc.BlendState = DX12::GetBlendState(BlendState::Disabled);
        psoDesc.DepthStencilState = DX12::GetDepthState(DepthState::Enabled);
        psoDesc.SampleMask = uint32(-1);
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = workloadRT.Format();
        psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        psoDesc.SampleDesc.Count = workloadRT.MSAASamples;
        psoDesc.SampleDesc.Quality = workloadRT.MSAAQuality;
        psoDesc.InputLayout.pInputElementDescs = nullptr;
        psoDesc.InputLayout.NumElements = 0;
        DXCall(DX12::Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&workloadGfxPSO)));
    }
}

void OverlappedExecution::DestroyPSOs()
{
    DX12::DeferredRelease(workloadCSPSO);
    DX12::DeferredRelease(workloadGfxPSO);
}

void OverlappedExecution::BeforeFlush()
{
    // Signal the throttling fence so that we don't hang
    waitFence.Clear(uint64(-1));

    // Flush the compute queue
    Assert_(DX12::CurrentCPUFrame >= currentComputeFrame);
    if(DX12::CurrentCPUFrame > currentComputeFrame)
    {
        computeFence.Wait(DX12::CurrentCPUFrame);
        currentComputeFrame = DX12::CurrentCPUFrame;
    }

}

void OverlappedExecution::Update(const Timer& timer)
{
    CPUProfileBlock profileBlock("Update (CPU)");

    if(DX12::CurrentCPUFrame > 0)
    {
        // Wait for the last present to finish
        WaitForSingleObjectEx(swapChain.WaitableObject(), 1000, false);

        // Let the GPU start working
        waitFence.Clear(DX12::CurrentCPUFrame);

        Workload* enabledWorkloads[NumWorkloads] = { };
        uint64 numEnabledWorkloads = 0;
        for(uint64 i = 0; i < NumWorkloads; ++i)
            if(workloads[i].Enabled && workloads[i].LastUpdatedFrame == DX12::CurrentCPUFrame - 1)
                enabledWorkloads[numEnabledWorkloads++] = &workloads[i];

        if(numEnabledWorkloads > 0)
            TimeWorkloads(enabledWorkloads, numEnabledWorkloads);
    }

    // Toggle VSYNC
    swapChain.SetVSYNCEnabled(AppSettings::EnableVSync ? true : false);

    if(AppSettings::StablePowerState.Changed())
        DX12::Device->SetStablePowerState(AppSettings::StablePowerState);
}

void OverlappedExecution::Render(const Timer& timer)
{
    ID3D12GraphicsCommandList* cmdList = DX12::CmdList;

    CPUProfileBlock profileBlock("Render (CPU)");
    ProfileBlock gpuProfileBlock(cmdList, "Frame Time");

    for(uint64 i = 0; i < NumWorkloads; ++i)
    {
        Workload& workload = workloads[i];
        workload.BufferIsReadable = false;
        if(workload.Enabled == false || workload.Type == WorkloadType::ComputeQueue)
            continue;

        // Clear the count buffer
        D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptors[1] = { workload.CounterBuffer.UAV() };
        DescriptorHandle gpuHandle = DX12::MakeDescriptorTable(ArraySize_(cpuDescriptors), cpuDescriptors);

        uint32 values[4] = { };
        cmdList->ClearUnorderedAccessViewUint(gpuHandle.GPUHandle, cpuDescriptors[0], workload.CounterBuffer.Resource(), values, 0, nullptr);

        workload.CounterBuffer.UAVBarrier(cmdList);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[1] = { workloadRT.RTV.CPUHandle };
    cmdList->OMSetRenderTargets(1, rtvHandles, false, nullptr);
    DX12::SetViewport(cmdList, workloadRT.Width(), workloadRT.Height());

    for(uint64 currWorkloadIdx = 0; currWorkloadIdx < NumWorkloads; ++currWorkloadIdx)
    {
        Workload& workload = workloads[currWorkloadIdx];
        if(workload.Enabled == false || workload.Type == WorkloadType::ComputeQueue)
            continue;

        // Handle dependencies
        if(workload.DependsOn < NumWorkloads && workloads[workload.DependsOn].Enabled && workloads[workload.DependsOn].BufferIsReadable == false)
        {
            Assert_(workload.DependsOn < currWorkloadIdx);
            Workload& dependency = workloads[workload.DependsOn];

            // Issue the barrier to synchronize
            D3D12_RESOURCE_BARRIER barrier = { };
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = dependency.CounterBuffer.Resource();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
            barrier.Transition.Subresource = 0;

            // For split barriers we're finishing the barrier here instead of issuing a new one
            if(AppSettings::UseSplitBarriers)
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;

            cmdList->ResourceBarrier(1, &barrier);

            dependency.BufferIsReadable = true;
        }

        if(workload.Type == WorkloadType::Compute)
            DoComputeWorkload(cmdList, workload);
        else if(workload.Type == WorkloadType::Graphics)
            DoGraphicsWorkload(workload);

        if(AppSettings::UseSplitBarriers)
        {
            // See if anybody else depends on this workload
            bool isDependedOn = false;
            for(uint64 otherIdx = currWorkloadIdx + 1; otherIdx < NumWorkloads; ++otherIdx)
            {
                if(workloads[otherIdx].Enabled && workloads[otherIdx].DependsOn == currWorkloadIdx)
                {
                    isDependedOn = true;
                    break;
                }
            }

            if(isDependedOn)
            {
                // Start the transition barrier
                D3D12_RESOURCE_BARRIER barrier = { };
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
                barrier.Transition.pResource = workload.CounterBuffer.Resource();
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
                barrier.Transition.Subresource = 0;
                cmdList->ResourceBarrier(1, &barrier);
            }
        }
    }

    rtvHandles[0] = { swapChain.BackBuffer().RTV.CPUHandle };
    cmdList->OMSetRenderTargets(1, rtvHandles, false, nullptr);
    DX12::SetViewport(cmdList, swapChain.Width(), swapChain.Height());

    const float clearColor[] = { 0.392f, 0.584f, 0.929f, 1.0f };
    cmdList->ClearRenderTargetView(rtvHandles[0], clearColor, 0, nullptr);

    RenderWorkloadUI();
    RenderHUD();

    // Tell the GPU to wait until we're ready to start timing shader executions
    DX12::GfxQueue->Wait(waitFence.D3DFence, DX12::CurrentCPUFrame + 1);

    RenderCompute();
}

void OverlappedExecution::RenderCompute()
{
    ID3D12GraphicsCommandList* cmdList = computeCmdList;

    DX12::SetDescriptorHeaps(cmdList);

    for(uint64 i = 0; i < NumWorkloads; ++i)
    {
        Workload& workload = workloads[i];
        workload.BufferIsReadable = false;
        if(workload.Enabled == false || workload.Type != WorkloadType::ComputeQueue)
            continue;

        // Clear the count buffer
        D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptors[1] = { workload.CounterBuffer.UAV() };
        DescriptorHandle gpuHandle = DX12::MakeDescriptorTable(ArraySize_(cpuDescriptors), cpuDescriptors);

        uint32 values[4] = { };
        cmdList->ClearUnorderedAccessViewUint(gpuHandle.GPUHandle, cpuDescriptors[0], workload.CounterBuffer.Resource(), values, 0, nullptr);

        workload.CounterBuffer.UAVBarrier(cmdList);
    }

    for(uint64 currWorkloadIdx = 0; currWorkloadIdx < NumWorkloads; ++currWorkloadIdx)
    {
        Workload& workload = workloads[currWorkloadIdx];
        if(workload.Enabled == false || workload.Type != WorkloadType::ComputeQueue)
            continue;

         // Handle dependencies
        if(workload.DependsOn < NumWorkloads && workloads[workload.DependsOn].Enabled && workloads[workload.DependsOn].BufferIsReadable == false)
        {
            Assert_(workload.DependsOn < currWorkloadIdx);
            Workload& dependency = workloads[workload.DependsOn];

            // Issue the barrier to synchronize
            D3D12_RESOURCE_BARRIER barrier = { };
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = dependency.CounterBuffer.Resource();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = 0;

            // For split barriers we're finishing the barrier here instead of issuing a new one
            if(AppSettings::UseSplitBarriers)
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;

            cmdList->ResourceBarrier(1, &barrier);

            dependency.BufferIsReadable = true;
        }

        DoComputeWorkload(cmdList, workload);

        if(AppSettings::UseSplitBarriers)
        {
            // See if anybody else depends on this workload
            bool isDependedOn = false;
            for(uint64 otherIdx = currWorkloadIdx + 1; otherIdx < NumWorkloads; ++otherIdx)
            {
                if(workloads[otherIdx].Enabled && workloads[otherIdx].DependsOn == currWorkloadIdx)
                {
                    isDependedOn = true;
                    break;
                }
            }

            if(isDependedOn)
            {
                // Start the transition barrier
                D3D12_RESOURCE_BARRIER barrier = { };
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
                barrier.Transition.pResource = workload.CounterBuffer.Resource();
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                barrier.Transition.Subresource = 0;
                cmdList->ResourceBarrier(1, &barrier);
            }
        }
    }

    // Tell the GPU to wait until we're ready to start timing shader executions
    ID3D12CommandQueue* queue = AppSettings::UseHiPriorityComputeQueue ? hiPriorityComputeQueue : computeQueue;
    queue->Wait(waitFence.D3DFence, DX12::CurrentCPUFrame + 1);

    // Submit on the compute queue
    DXCall(cmdList->Close());

    ID3D12CommandList* commandLists[] = { cmdList };
    queue->ExecuteCommandLists(ArraySize_(commandLists), commandLists);

    // Signal the fence with the current frame number, so that we can check back on it
    uint64 nextCPUFrame = DX12::CurrentCPUFrame + 1;
    computeFence.Signal(queue, nextCPUFrame);

    // Wait for the GPU to catch up before we stomp an executing command buffer
    const uint64 gpuLag = nextCPUFrame - currentComputeFrame;
    Assert_(gpuLag <= DX12::RenderLatency);
    if(gpuLag >= DX12::RenderLatency)
    {
        // Make sure that the previous frame is finished
        computeFence.Wait(currentComputeFrame + 1);
        ++currentComputeFrame;
    }

    uint64 nextFrameIdx = nextCPUFrame % DX12::RenderLatency;

    // Prepare the command buffers to be used for the next frame
    DXCall(cmdAllocators[nextFrameIdx]->Reset());
    DXCall(cmdList->Reset(cmdAllocators[nextFrameIdx], nullptr));
}

void OverlappedExecution::DoComputeWorkload(ID3D12GraphicsCommandList* cmdList, Workload& workload)
{
    PIXMarker pixMarker(cmdList, workload.Name);
    ProfileBlock workloadBlock(cmdList, workload.Name);

    cmdList->SetPipelineState(workloadCSPSO);
    cmdList->SetComputeRootSignature(workloadRootSignature);

    D3D12_CPU_DESCRIPTOR_HANDLE handles[] = { workloadInputBuffer.SRV(), workloadOutputBuffer.UAV(),
                                              workload.ShaderStartBuffer.UAV(), workload.ShaderEndBuffer.UAV(),
                                              workload.CounterBuffer.UAV() };
    DX12::BindShaderResources(cmdList, 0, ArraySize_(handles), handles, CmdListMode::Compute);

    const uint32 numWorkloadElements = workload.NumGroups * 1024;

    workloadCBuffer.Data.FrameNum = uint32(DX12::CurrentCPUFrame);
    workloadCBuffer.Data.NumIterations = workload.NumIterations;
    workloadCBuffer.Data.NumWorkloadElements = numWorkloadElements;
    workloadCBuffer.Upload();
    workloadCBuffer.SetAsComputeRootParameter(cmdList, 1);

    cmdList->Dispatch(DX12::DispatchSize(numWorkloadElements, 64), 1, 1);

    workload.LastUpdatedFrame = DX12::CurrentCPUFrame;
}

void OverlappedExecution::DoGraphicsWorkload(Workload& workload)
{
    ID3D12GraphicsCommandList* cmdList =  DX12::CmdList;

    PIXMarker pixMarker(cmdList, workload.Name);
    ProfileBlock workloadBlock(cmdList, workload.Name);

    DX12::SetViewport(cmdList, workloadRT.Width(), workload.NumGroups);

    cmdList->SetPipelineState(workloadGfxPSO);
    cmdList->SetGraphicsRootSignature(workloadRootSignature);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_CPU_DESCRIPTOR_HANDLE handles[] = { workloadInputBuffer.SRV(), workloadOutputBuffer.UAV(),
                                              workload.ShaderStartBuffer.UAV(), workload.ShaderEndBuffer.UAV(),
                                              workload.CounterBuffer.UAV() };
    DX12::BindShaderResources(cmdList, 0, ArraySize_(handles), handles, CmdListMode::Graphics);

    const uint32 numWorkloadElements = workload.NumGroups * 1024;

    workloadCBuffer.Data.FrameNum = uint32(DX12::CurrentCPUFrame);
    workloadCBuffer.Data.NumIterations = workload.NumIterations;
    workloadCBuffer.Data.NumWorkloadElements = numWorkloadElements;
    workloadCBuffer.Upload();
    workloadCBuffer.SetAsGfxRootParameter(cmdList, 1);

    cmdList->DrawInstanced(3, 1, 0, 0);

    workload.LastUpdatedFrame = DX12::CurrentCPUFrame;
}

void OverlappedExecution::RenderHUD()
{
    float width = float(swapChain.Width());
    float height = float(swapChain.Height());

    const float windowPercentage = 0.75f;
    Float2 windowSize = Float2(width, height) * windowPercentage;
    Float2 windowPos = Float2(width, height) * (1.0f - windowPercentage) * 0.5f;
    Float2 windowEnd = windowPos + windowSize;

    ImGui::SetNextWindowPos(ToImVec2(windowPos), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ToImVec2(windowSize), ImGuiSetCond_Always);
    ImGui::Begin("HUD Window", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Draw the timeline
    float timelineY = windowPos.y + 50.0f;
    float timelineStartX = windowPos.x + windowSize.x * 0.1f;
    float timelineEndX = windowPos.x + windowSize.x * 0.9f;
    float timelineWidth = timelineEndX - timelineStartX;
    uint32 timelineColor = ImColor(1.0f, 1.0f, 1.0f, 1.0f);
    drawList->AddLine(ImVec2(timelineStartX, timelineY), ImVec2(timelineEndX, timelineY), timelineColor);

    float tickStartY = timelineY;
    float majorTickEndY = tickStartY + 25.0f;
    float minorTickEndY = tickStartY + 12.5f;

    float actualFrameTime = 1000.0f / fps;
    float frameTime = actualFrameTime / AppSettings::TimelineZoom;

    uint64 numMajorTicks = uint64(std::floor(frameTime)) + 1;
    bool drawMinorTicks = (windowSize.x / numMajorTicks) >= 100.0f;
    for(uint64 tickIdx = 0; tickIdx < numMajorTicks; ++tickIdx)
    {
        float tickX = timelineStartX + timelineWidth * (tickIdx / frameTime);
        drawList->AddLine(ImVec2(tickX, tickStartY), ImVec2(tickX, majorTickEndY), timelineColor);

        std::string tickText = MakeString("%llu.0ms", tickIdx);
        ImVec2 textSize = ImGui::CalcTextSize(tickText.c_str());
        drawList->AddText(ImVec2(tickX - textSize.x * 0.5f, tickStartY - textSize.y - 2.0f), timelineColor, tickText.c_str());

        if(drawMinorTicks && (tickIdx + 0.5f) <= frameTime)
        {
            float minorTickX = timelineStartX + timelineWidth * ((tickIdx + 0.5f) / frameTime);
            drawList->AddLine(ImVec2(minorTickX, tickStartY), ImVec2(minorTickX, minorTickEndY), timelineColor);

            tickText = MakeString("%llu.5ms", tickIdx);
            textSize = ImGui::CalcTextSize(tickText.c_str());
            drawList->AddText(ImVec2(minorTickX - textSize.x * 0.5f, tickStartY - textSize.y - 2.0f), timelineColor, tickText.c_str());
        }
    }

    std::string adapterText = MakeString("Adapter: %ls", adapterDesc.Description);
    Float2 adapterTextSize = ToFloat2(ImGui::CalcTextSize(adapterText.c_str()));
    Float2 adapterTextPos = Float2(timelineStartX, timelineY - (adapterTextSize.y * 2.0f) - 12.0f);
    drawList->AddText(ToImVec2(adapterTextPos), timelineColor, adapterText.c_str());

    std::string frameTimeText = MakeString("Frame Time: %.2fms", actualFrameTime);
    Float2 frameTimeTextSize = ToFloat2(ImGui::CalcTextSize(frameTimeText.c_str()));
    Float2 frameTimeTextPos = Float2(timelineEndX - frameTimeTextSize.x, timelineY - (frameTimeTextSize.y * 2.0f) - 12.0f);
    drawList->AddText(ToImVec2(frameTimeTextPos), timelineColor, frameTimeText.c_str());

    const float barHeight = 75.0f;
    const float barStartY = timelineY + 25.0f;
    const uint32 barColor = ImColor(1.0f, 0.0f, 0.0f, 1.0f);
    const uint32 barOutlineColor = ImColor(1.0f, 1.0f, 1.0f, 1.0f);
    const uint32 barTextColor = ImColor(1.0f, 1.0f, 1.0f, 1.0f);

    // Draw the workloads
    for(uint64 workloadIdx = 0; workloadIdx < NumWorkloads; ++workloadIdx)
    {
        const Workload& workload = workloads[workloadIdx];
        if(workload.Enabled == false)
            continue;

        const float executionTime = workload.EndTime - workload.StartTime;
        Float2 barStart = Float2(timelineStartX + timelineWidth * (workload.StartTime / frameTime), barStartY + workloadIdx * barHeight);
        Float2 barSize = Float2(timelineWidth * (executionTime / frameTime), barHeight);
        Float2 barEnd = barStart + barSize;
        drawList->AddRectFilled(ToImVec2(barStart), ToImVec2(barEnd), barColor);
        drawList->AddRect(ToImVec2(barStart), ToImVec2(barEnd), barOutlineColor);

        std::string barText = MakeString("%s (%.2fms)", workload.Name, executionTime);
        Float2 textSize = ToFloat2(ImGui::CalcTextSize(barText.c_str(), nullptr, false, barSize.x));
        if(textSize.x < barSize.x && textSize.y < barSize.y)
        {
            Float2 textPos = barStart + (barSize * 0.5f) - (textSize * 0.5f);
            drawList->AddText(ImGui::GetWindowFont(), ImGui::GetWindowFontSize(), ToImVec2(textPos), barTextColor, barText.c_str(), nullptr, barSize.x);
        }

        if(workload.DependsOn < NumWorkloads && workloads[workload.DependsOn].Enabled)
        {
            const Workload& dependency = workloads[workload.DependsOn];
            const float dependencyExecutionTime = dependency.EndTime - dependency.StartTime;
            Float2 dependencyStart = Float2(timelineStartX + timelineWidth * (dependency.StartTime / frameTime), barStartY + workload.DependsOn * barHeight);
            Float2 dependencySize = Float2(timelineWidth * (dependencyExecutionTime / frameTime), barHeight);
            Float2 dependencyEndMidPoint = dependencyStart + dependencySize * Float2(1.0f, 0.5f);
            Float2 barStartMidPoint = barStart + Float2(0.0f, barSize.y * 0.5f);
            drawList->AddLine(ToImVec2(dependencyEndMidPoint), ToImVec2(barStartMidPoint), ImColor(1.0f, 1.0f, 0.0f, 1.0f));
        }
    }

    ImGui::End();
}

void OverlappedExecution::RenderWorkloadUI()
{
    if(AppSettings::ShowWorkloadUI == false)
        return;

    if(ImGui::Begin("Workload Settings", nullptr) == false)
    {
        ImGui::End();
        return;
    }

    const char* comboEntries[NumGfxQueueWorkloads + 1] = { };
    comboEntries[0] = "None";
    for(uint64 i = 0; i < NumGfxQueueWorkloads; ++i)
        comboEntries[i + 1] = WorkloadNames[i];

    const char* computeComboEntries[NumComputeQueueWorkloads + 1] = { };
    computeComboEntries[0] = "None";
    for(uint64 i = 0; i < NumComputeQueueWorkloads; ++i)
        computeComboEntries[i + 1] = WorkloadNames[i + ComputeQueueWorkloadA];

    for(uint64 i = 0; i < NumWorkloads; ++i)
    {
        Workload& workload = workloads[i];
        if(ImGui::CollapsingHeader(workload.Name, nullptr, true, true))
        {
            ImGui::PushID(workload.Name);

            ImGui::Checkbox("Enable", &workload.Enabled);
            ImGui::SliderInt("Num Groups", &workload.NumGroups, 1, AppSettings::MaxWorkloadGroups);
            ImGui::SliderInt("Num Iterations", &workload.NumIterations, 1, AppSettings::MaxWorkloadIterations);

            if(i > 0 && i < NumGfxQueueWorkloads)
            {
                int32 idx = int32(workload.DependsOn + 1);
                ImGui::Combo("Depends On", &idx, comboEntries, int32(i + 1));
                workload.DependsOn = uint64(idx - 1);
            }
            else if(i > ComputeQueueWorkloadA)
            {
                int32 idx = workload.DependsOn == uint64(-1) ? 0 : int32(workload.DependsOn + 1 - NumGfxQueueWorkloads);
                ImGui::Combo("Depends On", &idx, computeComboEntries, int32(i + 1 - NumGfxQueueWorkloads));
                workload.DependsOn = idx == 0 ? uint64(-1) : idx + NumGfxQueueWorkloads - 1;
            }

            ImGui::PopID();
        }
    }

    ImGui::End();
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    OverlappedExecution app(lpCmdLine);
    app.Run();
}
