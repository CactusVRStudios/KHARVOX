using System.Runtime.InteropServices;

namespace KharvoxLauncher;

internal static class VrIntroHost
{
    // Resolve the packaged native entry point through the CLR. Do not search
    // the working directory, PATH, or process-added DLL directories.
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory |
        DllImportSearchPath.UseDllDirectoryForDependencies | DllImportSearchPath.System32)]
    [DllImport("KharvoxLayer.dll", EntryPoint = "KharvoxRunVrIntro",
        CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int RunIntro([MarshalAs(UnmanagedType.LPWStr)] string? token, uint parentId);

    internal static int Run(string[] args)
    {
        int result = 1;
        var thread = new System.Threading.Thread(() => result = RunCore(args));
        thread.SetApartmentState(System.Threading.ApartmentState.MTA);
        thread.Start(); thread.Join(); return result;
    }

    private static int RunCore(string[] args)
    {
        try
        {
            string? token = null; uint parent = 0;
            if (args.Length == 2 && args[0].Length == 32 &&
                args[0].All(c => "0123456789abcdef".Contains(c)) && uint.TryParse(args[1], out parent)) token = args[0];
            else if (args.Length != 0) throw new ArgumentException("Invalid VR intro arguments.");
            var path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "KharvoxLayer.dll");
            if (!File.Exists(path)) throw new InvalidOperationException("Could not find KharvoxLayer.dll. Extract the complete KHARVOX package.");
            // The CLR keeps the native module loaded until this dedicated process exits.
            return RunIntro(token, parent);
        }
        catch (Exception error)
        {
            try { File.AppendAllText(Path.Combine(Path.GetTempPath(), "KHARVOX-VR-INTRO.log"), error + Environment.NewLine); }
            catch { /* A logging failure must not prevent a controlled intro exit. */ }
            if (args.Length == 0) MessageBox.Show(error.Message, "KHARVOX - VR intro", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }
}
