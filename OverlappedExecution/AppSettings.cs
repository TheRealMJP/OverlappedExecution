public class Settings
{
    const int MaxWorkloadElements = 1024 * 256;
    const int MaxWorkloadIterations = 128;
    const int WorkloadGroupSize = 1024;
    const int WorkloadRTWidth = WorkloadGroupSize;
    const int MaxWorkloadGroups = MaxWorkloadElements / WorkloadGroupSize;

    [ExpandGroup(true)]
    public class General
    {
        [UseAsShaderConstant(false)]
        [DisplayName("Enable VSync")]
        [HelpText("Enables or disables vertical sync during Present")]
        bool EnableVSync = true;

        [UseAsShaderConstant(false)]
        [MinValue(1.0f)]
        [MaxValue(16.0f)]
        float TimelineZoom = 1.0f;

        [UseAsShaderConstant(false)]
        bool UseSplitBarriers = false;

        [UseAsShaderConstant(false)]
        bool StablePowerState = false;

        [UseAsShaderConstant(false)]
        bool UseHiPriorityComputeQueue = false;

        [UseAsShaderConstant(false)]
        bool ShowWorkloadUI = true;
    }
}