namespace KharvoxLauncher;
internal sealed class HeadsetUnavailableException : InvalidOperationException {
    internal const int ExitCode=0x4b480002;
    internal HeadsetUnavailableException() : base("No usable OpenXR headset is available.\n\nConnect your headset and start its OpenXR runtime, then try again. If you use Virtual Desktop, connect to your PC first.") { }
}
