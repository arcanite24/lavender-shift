using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace LavenderShift.Launcher;

/// <summary>Starts lavender_shift.exe itself, with no script in between. The
/// game finds its disc, update and user folders beside it (src/install_paths.cpp),
/// so the launcher only names the user folder, and save sync lives in the
/// game's settings file, so a game started any other way syncs too.</summary>
public static class GameStarter
{
    private const string SyncKey = "lavender_sync_folder";

    public static string GameSettingsPath(InstallLayout layout) =>
        Path.Combine(layout.UserRoot, "config", "lavender_shift.toml");

    /// <summary>Whether setup's ahead-of-time pipelines match this source's
    /// catalog (the check tools/prepare_shaders.ps1 makes).</summary>
    public static bool ShadersCurrent(InstallLayout layout)
    {
        try
        {
            var cache = Path.Combine(layout.UserRoot, "cache");
            var record = Path.Combine(cache, "aot-pipelines.json");
            if (!File.Exists(Path.Combine(cache, "shaders", "shareable", "4D530AA4.fbo.aot.xpso")) || !File.Exists(record))
                return false;
            using var stream = File.OpenRead(Path.Combine(layout.SourceRoot, "config", "aot-pipelines.json"));
            var key = Convert.ToHexString(SHA256.HashData(stream));
            using var state = JsonDocument.Parse(File.ReadAllText(record));
            return state.RootElement.TryGetProperty("catalog", out var catalog) &&
                string.Equals(catalog.GetString(), key, StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    /// <summary>Puts the sync folder (or off) in the game's settings file.
    /// False when the game has not written that file yet: it writes its
    /// first-run defaults only when it creates the file, so the launcher
    /// never creates it.</summary>
    public static bool WriteSyncSetting(InstallLayout layout, string? folder)
    {
        var path = GameSettingsPath(layout);
        if (!File.Exists(path)) return false;
        try
        {
            var text = File.ReadAllText(path);
            // Forward slashes: no escapes for the game's reader or TOML's.
            var line = $"{SyncKey} = \"{folder?.Replace('\\', '/') ?? ""}\"";
            var pattern = new Regex($@"^[ \t]*{SyncKey}[ \t]*=.*?(?=\r?$)", RegexOptions.Multiline);
            if (pattern.IsMatch(text))
            {
                text = pattern.Replace(text, line.Replace("$", "$$"), 1);
            }
            else
            {
                var newline = text.Contains("\r\n") ? "\r\n" : "\n";
                text = text.TrimEnd('\r', '\n') + newline + line + newline;
            }
            var temporary = path + ".launcher";
            File.WriteAllText(temporary, text);
            File.Move(temporary, path, overwrite: true);
            return true;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    public static bool GameIsRunning()
    {
        var processes = Process.GetProcessesByName("lavender_shift");
        foreach (var process in processes) process.Dispose();
        return processes.Length > 0;
    }

    /// <summary>Starts the game. Throws when the executable cannot start.</summary>
    public static Process Start(InstallLayout layout, string? syncFolder)
    {
        var exe = layout.GameExecutable;
        if (!File.Exists(exe)) throw new FileNotFoundException("The game is not built. Run setup again.", exe);
        var start = new ProcessStartInfo(exe)
        {
            WorkingDirectory = Path.GetDirectoryName(exe)!,
            UseShellExecute = false,
        };
        start.ArgumentList.Add("--user_data_root=" + layout.UserRoot);
        // Before the game's first run there is no settings file to hold it.
        if (!WriteSyncSetting(layout, syncFolder) && syncFolder is not null)
            start.ArgumentList.Add("--" + SyncKey + "=" + syncFolder);
        return Process.Start(start) ?? throw new InvalidOperationException("The game did not start.");
    }

    /// <summary>Opening the launcher once the game is built starts the game
    /// and closes the launcher, unless Shift is held or --launcher is given.
    /// False (show the launcher) when setup is needed, the shaders need
    /// preparing, the game is already running or it quits at once.</summary>
    public static bool TryQuickStart()
    {
        InstallLayout layout;
        try { layout = InstallLayout.Locate(); }
        catch (FileNotFoundException) { return false; }
        if (!layout.IsReady() || GameIsRunning() || !ShadersCurrent(layout)) return false;
        var settings = LauncherSettings.Load(layout.SettingsPath);
        try
        {
            using var game = Start(layout, settings.SyncFolder);
            // A game that quits at once (a broken build, a missing driver)
            // leaves the launcher open, where Play reports it.
            return !game.WaitForExit(TimeSpan.FromSeconds(3)) || game.ExitCode == 0;
        }
        catch (Exception ex) when (ex is IOException or InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            return false;
        }
    }
}
