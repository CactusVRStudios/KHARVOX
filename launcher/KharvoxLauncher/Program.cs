namespace KharvoxLauncher;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--vr-intro") return VrIntroHost.Run(args.Skip(1).ToArray());

        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        if (args.Any(argument => string.Equals(argument, "--self-test", StringComparison.OrdinalIgnoreCase)))
            return SelfTest.Run();

        if (args.Any(argument => string.Equals(argument, "--intro", StringComparison.OrdinalIgnoreCase)))
        {
            using var intro = new CracktroForm();
            Application.Run(intro);
            return 0;
        }

        KharvoxRunner.DisableLayerRegistrationsOnExit();
        AppDomain.CurrentDomain.ProcessExit += (_, _) => KharvoxRunner.DisableLayerRegistrationsOnExit();
        Application.ApplicationExit += (_, _) => KharvoxRunner.DisableLayerRegistrationsOnExit();

        if (args.Any(argument => string.Equals(argument, "-run", StringComparison.OrdinalIgnoreCase)))
            return RunSavedSettings();

        Application.Run(new MainForm());
        return 0;
    }

    private static int RunSavedSettings()
    {
        var logPath = Path.Combine(Path.GetTempPath(), "KHARVOX-launcher-run.log");
        var logLock = new object();
        void Log(string message)
        {
            try
            {
                lock (logLock)
                    File.AppendAllText(logPath, $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] {message}{Environment.NewLine}");
            }
            catch { /* The regular KHARVOX.log remains available if this optional log cannot be written. */ }
        }

        try { File.WriteAllText(logPath, string.Empty); } catch { }
        Log($"Launcher build={KharvoxRunner.BuildId} command=-run");

        try
        {
            KharvoxLaunchOptions options;
            using (var form = new MainForm())
                options = form.CreateLaunchOptions();

            Log("Loaded existing settings: " + options.DiagnosticSummary());
            // Constructing WinForms controls installs a synchronization context.
            // The headless path has no message loop to service continuations, so
            // run the existing asynchronous launcher on the thread pool.
            Task.Run(() => KharvoxRunner.LaunchAsync(options,
                    () => Log("Startup result=healthy; launcher is closing and DOOM remains running."),
                    message => Log("Status: " + message)))
                .GetAwaiter().GetResult();
            if (options.EnableBhaptics)
            {
                Log("bHaptics enabled; launcher remains as bridge owner until DOOM exits.");
                while (KharvoxRunner.IsRunning) Thread.Sleep(500);
                KharvoxRunner.StopBhapticsAsync().GetAwaiter().GetResult();
            }
            Log("ExitCode=0");
            return 0;
        }
        catch (Exception ex)
        {
            Log("Startup result=failed");
            Log(ex.ToString());
            Log("ExitCode=1");
            if (ex is HeadsetUnavailableException)
                MessageBox.Show(ex.Message, "KHARVOX – Headset unavailable", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            if (ex is RenderMemoryCapacityException)
                MessageBox.Show(ex.Message, "KHARVOX â€“ Render Scale too high",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }
}
