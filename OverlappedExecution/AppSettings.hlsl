struct AppSettings_Layout
{
};

ConstantBuffer<AppSettings_Layout> AppSettings : register(b12);

static const int MaxWorkloadElements = 262144;
static const int MaxWorkloadIterations = 128;
static const int WorkloadGroupSize = 1024;
static const int WorkloadRTWidth = 1024;
static const int MaxWorkloadGroups = 256;
