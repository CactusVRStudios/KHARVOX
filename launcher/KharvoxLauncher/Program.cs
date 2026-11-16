namespace KharvoxLauncher;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--vr-intro") return VrIntroHost.Run(args.Skip(1).ToArray());

        if (args.Length == 2 && args[0] == "--verify-game")
        {
            var report = Path.Combine(Path.GetTempPath(), "KHARVOX-game-build-check.log");
            try { File.WriteAllText(report, DoomBuildProfile.Inspect(args[1]).ToString()); return 0; }
            catch (Exception e) { File.WriteAllText(report, e.ToString()); return 1; }
        }

        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        if (args.Any(argument => string.Equals(argument, "--self-test", StringComparison.OrdinalIgnoreCase)))
            return SelfTest.Run();

        if (args.Length == 3 && args[0] == "--desktop-options")
        {
            try
            {
                using var form = new MainForm(args[1]);
                File.WriteAllText(args[2], new System.Web.Script.Serialization.JavaScriptSerializer()
                    .Serialize(form.CreateLaunchOptions()));
                return 0;
            }
            catch (Exception error) { File.WriteAllText(args[2], error.ToString()); return 1; }
        }

        if (args.Any(argument => string.Equals(argument, "--intro", StringComparison.OrdinalIgnoreCase)))
        {
            using var intro = new CracktroForm();
            Application.Run(intro);
            return 0;
        }

        KharvoxRunner.DisableLayerRegistrationsOnExit();
        AppDomain.CurrentDomain.ProcessExit += (_, _) => KharvoxRunner.DisableLayerRegistrationsOnExit();
        Application.ApplicationExit += (_, _) => KharvoxRunner.DisableLayerRegistrationsOnExit();

        if (args.Length == 4 && args[0] == "--desktop-run")
            return RunDesktopSession(args[1], args[2], args[3]);

        if (args.Any(argument => string.Equals(argument, "-run", StringComparison.OrdinalIgnoreCase)))
            return RunSavedSettings();

        Application.Run(new MainForm());
        return 0;
    }

    private static int RunDesktopSession(string settingsPath, string statusPath, string controlPath)
    {
        void Status(string state, string text)
        {
            try { File.WriteAllText(statusPath, state + "\n" + text); }
            catch { /* A closed desktop UI must not stop the running game. */ }
        }
        try
        {
            KharvoxLaunchOptions options;
            using (var form = new MainForm(settingsPath)) options = form.CreateLaunchOptions();
            // Control construction installs a WinForms synchronization context;
            // the background backend has no visible message loop to service it.
            Task.Run(async () => {
                await KharvoxRunner.LaunchAsync(options,
                    () => Status("RUNNING", "Jeu en cours — le casque est prêt."),
                    message => Status("STARTING", message)).ConfigureAwait(false);
                while (KharvoxRunner.IsRunning)
                {
                    if (File.Exists(controlPath) && File.ReadAllText(controlPath).Trim() == "stop")
                    {
                        Status("STOPPING", "Fermeture de DOOM…");
                        await KharvoxRunner.EndDoomAsync().ConfigureAwait(false);
                        break;
                    }
                    await Task.Delay(250).ConfigureAwait(false);
                }
                await KharvoxRunner.StopBhapticsAsync().ConfigureAwait(false);
                await KharvoxRunner.StopPsvr2Async().ConfigureAwait(false);
            }).GetAwaiter().GetResult();
            Status("STOPPED", "Session terminée. Prêt pour la prochaine.");
            return 0;
        }
        catch (Exception error)
        {
            Status("ERROR", error.Message);
            try { File.AppendAllText(Path.Combine(Path.GetTempPath(), "KHARVOX-desktop-errors.log"), error + Environment.NewLine); } catch { }
            return 1;
        }
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
                MessageBox.Show(ex.Message, "KHARVOX – Render Scale too high",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }
}
