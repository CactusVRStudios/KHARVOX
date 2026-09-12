namespace KharvoxLauncher;

internal sealed class RenderMemoryCapacityException : InvalidOperationException
{
    // Matched by EngineMemoryCapacity.h. Classify this process's exit status,
    // never a previous launch's log or a generic Vulkan allocation failure.
    internal const int ExitCode = 0x4b480001;
    internal const string UserMessage =
        "The selected render resolution exceeds DOOM's memory pool block capacity.\n\n" +
        "Reduce Render Scale and try again. Your selected settings have not been changed.";

    internal RenderMemoryCapacityException() : base(UserMessage) { }
}
