using System.Web.Script.Serialization;

namespace KharvoxLauncher;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length == 3 && args[0] == "--desktop-options")
        {
            try
            {
                var options = SettingsLaunchOptions.From(LauncherSettingsStore.Load(args[1]));
                File.WriteAllText(args[2], new JavaScriptSerializer().Serialize(options));
                return 0;
            }
            catch (Exception error) { File.WriteAllText(args[2], error.ToString()); return 1; }
        }
        if (args.Length == 2 && args[0] == "--verify-game")
        {
            try { File.WriteAllText(Path.Combine(Path.GetTempPath(), "KHARVOX-game-build-check.log"), DoomBuildProfile.Inspect(args[1]).ToString()); return 0; }
            catch { return 1; }
        }
        if (args.Length != 4 || args[0] != "--desktop-run") return 2;
        var statusPath = args[2];
        void Status(string state, string text)
        {
            try { File.WriteAllText(statusPath, state + "\n" + text); } catch { }
        }
        KharvoxRunner.DisableLayerRegistrationsOnExit();
        AppDomain.CurrentDomain.ProcessExit += (_, _) => KharvoxRunner.DisableLayerRegistrationsOnExit();
        try
        {
            var options = SettingsLaunchOptions.From(LauncherSettingsStore.Load(args[1]));
            Task.Run(async () => {
                await KharvoxRunner.LaunchAsync(options,
                    () => Status("RUNNING", "Jeu en cours — le casque est prêt."),
                    text => Status("STARTING", text)).ConfigureAwait(false);
                while (KharvoxRunner.IsRunning)
                {
                    if (File.Exists(args[3]) && File.ReadAllText(args[3]).Trim() == "stop")
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
}
