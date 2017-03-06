//=================================================================================================
//
//  Overlapped Execution Sample
//  by MJP
//  http://mynameismjp.wordpress.com/
//
//  All code and content licensed under the MIT license
//
//=================================================================================================

#include "Noise.hlsl"

// Inputs
StructuredBuffer<float4> InputBuffer : register(t0);

// Outputs
RWStructuredBuffer<float> OutputBuffer : register(u0);
RWStructuredBuffer<uint> ShaderStartBuffer : register(u1);
RWStructuredBuffer<uint> ShaderEndBuffer : register(u2);
RWByteAddressBuffer CounterBuffer : register(u3);

struct WorkloadConstants
{
    uint FrameNum;
    uint NumIterations;
    uint NumWorkloadElements;
};

ConstantBuffer<WorkloadConstants> WorkloadCBuffer : register(b0);

float DoWorkload(in uint workloadIdx)
{
    uint prevValue = 0;
    CounterBuffer.InterlockedAdd(0, 1, prevValue);
    if(prevValue == 0)
        ShaderStartBuffer[0] = WorkloadCBuffer.FrameNum;

    float sum = 0.0f;
    for(uint i = 0; i < WorkloadCBuffer.NumIterations; ++i)
    {
        uint elemIdx = i * WorkloadCBuffer.NumWorkloadElements + workloadIdx;
        float currNoise = cnoise(InputBuffer[elemIdx]);
        OutputBuffer[elemIdx] = currNoise;
        sum += currNoise;
    }

    if(prevValue + 1 == WorkloadCBuffer.NumWorkloadElements)
        ShaderEndBuffer[0] = WorkloadCBuffer.FrameNum;

    return sum;
}

[numthreads(64, 1, 1)]
void WorkloadCS(in uint3 GroupID : SV_GroupID, in uint3 GroupThreadID : SV_GroupThreadID)
{
    uint workloadIdx = GroupID.x * 64 + GroupThreadID.x;
    DoWorkload(workloadIdx);
}

float4 WorkloadVS(in uint VertexIdx : SV_VertexID) : SV_Position
{
    if(VertexIdx == 0)
        return float4(-1.0f, 1.0f, 1.0f, 1.0f);
    else if(VertexIdx == 1)
        return float4(3.0f, 1.0f, 1.0f, 1.0f);
    else
        return float4(-1.0f, -3.0f, 1.0f, 1.0f);
}

float4 WorkloadPS(in float4 PixelPos : SV_Position) : SV_Target0
{
    uint workloadIdx = uint(PixelPos.y) * 1024 + uint(PixelPos.x);
    return DoWorkload(workloadIdx);
}