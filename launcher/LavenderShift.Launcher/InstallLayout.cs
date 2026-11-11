using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;

namespace LavenderShift.Launcher;

/// <summary>Where everything lives (RL-3). A release keeps every file under
/// the folder the launcher runs from, so moving or removing that folder is
/// the whole install:
///   LavenderShiftLauncher.exe, lavender-shift-source.zip  (the release ZIP)
///   source\        the extracted source, its toolchain, game files and build
///   user\          saves, settings, cache and game logs (never touched by updates)
///   launcher.json  the files the player chose
/// The source folder keeps its name across releases, so an update rebuilds
/// only what changed and keeps the extracted game files. Run from a checkout
/// (no payload beside the exe), the launcher uses that checkout and its
/// .local\user.</summary>
public sealed class InstallLayout
{
    public const string PayloadName = "lavender-shift-source.zip";
    private const string HashMarker = ".lavender-source-sha256";

    public required string InstallRoot { get; init; }
    public required string SourceRoot { get; init; }
    public required string UserRoot { get; init; }
    public string? Payload { get; init; }
    public bool IsCheckout => Payload is null;

    public string SettingsPath => Path.Combine(InstallRoot, "launcher.json");
    public string LogsRoot => Path.Combine(SourceRoot, ".local", "logs");
    public string SetupErrorPath => Path.Combine(LogsRoot, "setup-error.json");
    public string SetupStatePath => Path.Combine(SourceRoot, ".local", "setup-state.json");
    public string GameExecutable => Path.Combine(SourceRoot, "out", "build", "win-amd64-release", "lavender_shift.exe");
    public string Tool(string name) => Path.Combine(SourceRoot, "tools", name);

    public static string Version =>
        typeof(InstallLayout).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()
            ?.InformationalVersion.Split('+')[0] ?? "0.0.0";

    public static InstallLayout Locate()
    {
        var exeDir = Path.GetFullPath(AppContext.BaseDirectory).TrimEnd(Path.DirectorySeparatorChar);
        var payload = Path.Combine(exeDir, PayloadName);
        if (File.Exists(payload))
        {
            return new InstallLayout
            {
                InstallRoot = exeDir,
                SourceRoot = Path.Combine(exeDir, "source"),
                UserRoot = Path.Combine(exeDir, "user"),
                Payload = payload,
            };
        }
        for (var dir = new DirectoryInfo(exeDir); dir is not null; dir = dir.Parent)
        {
            if (File.Exists(Path.Combine(dir.FullName, "tools", "setup-release.ps1")) &&
                File.Exists(Path.Combine(dir.FullName, "lavender_shift_manifest.toml")))
            {
                return new InstallLayout
                {
                    InstallRoot = Path.Combine(dir.FullName, ".local"),
                    SourceRoot = dir.FullName,
                    UserRoot = Path.Combine(dir.FullName, ".local", "user"),
                };
            }
        }
        throw new FileNotFoundException(
            $"{PayloadName} is missing. Extract the whole release ZIP into one folder and start the launcher from there.");
    }

    /// <summary>Whether the source folder holds the payload beside the exe.</summary>
    public bool SourceIsCurrent()
    {
        if (Payload is null) return true;
        var marker = Path.Combine(SourceRoot, HashMarker);
        return File.Exists(marker) && File.ReadAllText(marker).Trim() == PayloadHash();
    }

    /// <summary>Unpacks the payload over the source folder. Files the previous
    /// payload listed and this one drops are removed; .local (game files,
    /// toolchain) and out (the build) are left alone.</summary>
    public int ExtractPayload()
    {
        if (Payload is null) return 0;
        Directory.CreateDirectory(SourceRoot);
        IReadOnlyList<string> entries;
        using (var archive = ZipFile.OpenRead(Payload)) entries = PayloadManifest.Entries(archive);
        PayloadManifest.RemoveStale(SourceRoot, entries);
        var written = PayloadExtractor.Extract(Payload, SourceRoot);
        PayloadManifest.Write(SourceRoot, entries);
        File.WriteAllText(Path.Combine(SourceRoot, HashMarker), PayloadHash() + "\n");
        return written;
    }

    private string? payloadHash;
    private string PayloadHash()
    {
        if (payloadHash is not null) return payloadHash;
        using var stream = File.OpenRead(Payload!);
        return payloadHash = Convert.ToHexString(SHA256.HashData(stream));
    }

    /// <summary>Whether this release pins a prebuilt ShiftGlue kit (RL-10).
    /// Only then can setup use Microsoft's C++ libraries fetched by xwin in
    /// place of the Build Tools, so only then does the launcher offer it.</summary>
    public bool PinsPrebuiltKit()
    {
        const string entry = "config/release-toolchain.json";
        try
        {
            string json;
            if (Payload is not null)
            {
                using var archive = ZipFile.OpenRead(Payload);
                var config = archive.GetEntry(entry);
                if (config is null) return false;
                using var reader = new StreamReader(config.Open());
                json = reader.ReadToEnd();
            }
            else json = File.ReadAllText(Path.Combine(SourceRoot, entry));
            using var document = JsonDocument.Parse(json);
            return document.RootElement.TryGetProperty("shiftglue_kit", out var kits) &&
                kits.TryGetProperty("windows-x64", out _);
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or JsonException) { return false; }
    }

    /// <summary>An earlier setup finished, but the payload beside the exe is
    /// newer than the source it built.</summary>
    public bool IsReadyExceptSource() => Payload is not null && File.Exists(SetupStatePath) && !SourceIsCurrent();

    /// <summary>Setup finished for this source and the build is in place.</summary>
    public bool IsReady()
    {
        if (!File.Exists(SetupStatePath) || !File.Exists(GameExecutable) || !SourceIsCurrent()) return false;
        try
        {
            using var state = JsonDocument.Parse(File.ReadAllText(SetupStatePath));
            if (!state.RootElement.TryGetProperty("result", out var result) || result.GetString() != "ready")
                return false;
        }
        catch (JsonException) { return false; }
        // A new payload needs setup to run again (it rebuilds what changed).
        return Payload is null ||
            File.GetLastWriteTimeUtc(SetupStatePath) >= File.GetLastWriteTimeUtc(Path.Combine(SourceRoot, HashMarker));
    }
}

/// <summary>The files the player chose, kept beside the install.</summary>
public sealed class LauncherSettings
{
    public string? DiscImage { get; set; }
    public string? TitleUpdate { get; set; }
    public bool OwnershipConfirmed { get; set; }
    public bool MicrosoftLicenseAccepted { get; set; }

    private static readonly JsonSerializerOptions Options = new() { WriteIndented = true };

    public static LauncherSettings Load(string path)
    {
        try
        {
            if (File.Exists(path))
                return JsonSerializer.Deserialize<LauncherSettings>(File.ReadAllText(path)) ?? new();
        }
        catch (Exception ex) when (ex is JsonException or IOException) { }
        return new();
    }

    public void Save(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, JsonSerializer.Serialize(this, Options));
    }
}
