using System.IO;
using System.IO.Compression;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace LavenderShift.Launcher.Sharing;

// What the Android share offers.
public sealed record ShareContent(IReadOnlyList<ShareGroup> Groups);

// What the device can copy from this PC, into the layout
// LavenderShiftActivity plays from (under one of the app's external files
// folders):
//   game      SourceRoot\.local\game\disc and \update  -> game/disc, game/update
//   dlc       UserRoot\0000000000000000\4D530AA4       -> user/0000000000000000/4D530AA4
//   pipelines UserRoot\cache\shaders\shareable\4D530AA4.fbo.aot.xpso
//                                                      -> user/cache/shaders/shareable/
//   save      UserRoot\<profile>\..., achievements     -> user/<profile>/..., user/achievements
//   settings  UserRoot\config\lavender_shift.toml       -> user/config/lavender_shift.toml
// The save and the settings are offered unticked: the device backs up its
// own before they are replaced.
public static class AndroidShareContent
{
    public const string TitleId = "4D530AA4";
    public const string AotPipelines = TitleId + ".fbo.aot.xpso";
    public const string SettingsFile = "lavender_shift.toml";
    // The folder of content every profile shares (DLC packages).
    private const string SharedProfile = "0000000000000000";
    private static readonly Regex ProfileFolder = new("^[0-9A-Fa-f]{16}$");

    public static ShareContent Build(string sourceRoot, string userRoot)
    {
        var groups = new List<ShareGroup>();
        var game = Path.Combine(sourceRoot, ".local", "game");
        var disc = Path.Combine(game, "disc");
        if (!File.Exists(Path.Combine(disc, "default.xex")))
            throw new IOException($"The extracted game is not at {disc}. Set up and play on this PC first.");
        var gameFiles = Tree(disc, "game/disc").Concat(Tree(Path.Combine(game, "update"), "game/update")).ToList();
        groups.Add(new ShareGroup("game", "game files", true, gameFiles));

        var dlc = Tree(Path.Combine(userRoot, SharedProfile, TitleId), $"user/{SharedProfile}/{TitleId}");
        if (dlc.Count > 0) groups.Add(new ShareGroup("dlc", "DLC", true, dlc));

        // Free roam's Vulkan pipelines, built on this PC from the same disc;
        // the device builds them behind the title.
        var pipelines = Tree(Path.Combine(userRoot, "cache", "shaders", "shareable"), "user/cache/shaders/shareable",
            name => name == AotPipelines);
        if (pipelines.Count > 0) groups.Add(new ShareGroup("pipelines", "free-roam pipelines", true, pipelines));

        // The profiles' saves and the achievements unlocked with them.
        var save = new List<ShareFile>();
        if (Directory.Exists(userRoot))
        {
            foreach (var profile in Directory.EnumerateDirectories(userRoot).Select(Path.GetFileName).OfType<string>()
                         .Where(name => ProfileFolder.IsMatch(name) && name != SharedProfile).Order(StringComparer.Ordinal))
                save.AddRange(Tree(Path.Combine(userRoot, profile), $"user/{profile}"));
        }
        if (save.Count > 0)
        {
            save.AddRange(Tree(Path.Combine(userRoot, "achievements"), "user/achievements"));
            groups.Add(new ShareGroup("save", "this PC's save", false, save));
        }

        // The game's settings file only: its backups and the device's own
        // launch arguments (user/config/android-arguments.txt) stay.
        var settings = Tree(Path.Combine(userRoot, "config"), "user/config", name => name == SettingsFile);
        if (settings.Count > 0) groups.Add(new ShareGroup("settings", "this PC's settings", false, settings));

        return new ShareContent(groups);
    }

    private static IReadOnlyList<ShareFile> Tree(string source, string destination, Func<string, bool>? include = null)
    {
        if (!Directory.Exists(source)) return [];
        return Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories)
            .Select(path => (path, relative: Path.GetRelativePath(source, path).Replace('\\', '/')))
            .Where(entry => include?.Invoke(entry.relative) ?? true)
            .Select(entry => new ShareFile(entry.path, destination + "/" + entry.relative, new FileInfo(entry.path).Length))
            .ToList();
    }
}

// The Android package's build, from the provenance tools/lavender_android.py
// packs as assets/lavender_shift_build.json (version, commit, built_utc).
// The app compares it with its own to offer the PC's package as an update.
public sealed record AndroidApkInfo(string Version, long VersionCode, string Build, string? BuiltUtc, long Bytes)
{
    public static AndroidApkInfo? TryRead(string apk)
    {
        try
        {
            using var zip = ZipFile.OpenRead(apk);
            var entry = zip.GetEntry("assets/lavender_shift_build.json");
            if (entry is null) return null;
            using var stream = entry.Open();
            using var document = JsonDocument.Parse(stream);
            var root = document.RootElement;
            var version = root.GetProperty("version").GetString() ?? "";
            var build = root.TryGetProperty("commit", out var commit) ? commit.GetString() ?? "" : "";
            var built = root.TryGetProperty("built_utc", out var time) ? time.GetString() : null;
            return new AndroidApkInfo(version, CodeOf(version), build, built, new FileInfo(apk).Length);
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or JsonException or
                                       KeyNotFoundException or InvalidOperationException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    // The package's versionCode, as lavender_android.py _version() derives
    // it from the version name: major * 10000 + minor * 100 + patch.
    public static long CodeOf(string version)
    {
        var parts = version.Split('-')[0].Split('.');
        if (parts.Length < 3 || !int.TryParse(parts[0], out var major) || !int.TryParse(parts[1], out var minor) ||
            !int.TryParse(parts[2], out var patch))
            return 0;
        return major * 10000L + minor * 100 + patch;
    }
}
