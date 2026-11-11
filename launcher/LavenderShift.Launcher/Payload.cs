using System.IO;
using System.IO.Compression;

namespace LavenderShift.Launcher;

// Ported from Pinyon Shift's launcher (launcher/Shared/LauncherUpdate.cs).

/// <summary>The list of files the installed source payload put in the source
/// folder (.lavender-source-files). Extracting a new payload overwrites files
/// but never removes any, so files a release dropped are removed by this
/// list. Only files the previous payload listed are touched: .local (game
/// files, saves, settings), the build and anything else stay.</summary>
public static class PayloadManifest
{
    public const string FileName = ".lavender-source-files";

    public static IReadOnlyList<string> Entries(ZipArchive archive) =>
        archive.Entries.Where(entry => !string.IsNullOrEmpty(entry.Name))
            .Select(entry => entry.FullName.Replace('\\', '/')).Order(StringComparer.Ordinal).ToList();

    /// <summary>Deletes files the previous payload listed that the new one
    /// does not; returns their relative paths.</summary>
    public static IReadOnlyList<string> RemoveStale(string destination, IReadOnlyCollection<string> entries)
    {
        var manifest = Path.Combine(destination, FileName);
        if (!File.Exists(manifest)) return [];
        var keep = new HashSet<string>(entries, StringComparer.OrdinalIgnoreCase);
        var root = Path.GetFullPath(destination).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var removed = new List<string>();
        foreach (var line in File.ReadAllLines(manifest))
        {
            var relative = line.Trim().Replace('\\', '/');
            if (relative.Length == 0 || keep.Contains(relative)) continue;
            var first = relative.Split('/')[0];
            if (first is ".local" or "out" or ".." || relative.StartsWith('/')) continue;
            var path = Path.GetFullPath(Path.Combine(destination, relative));
            if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase) || !File.Exists(path)) continue;
            File.Delete(path);
            removed.Add(relative);
        }
        return removed;
    }

    public static void Write(string destination, IReadOnlyList<string> entries) =>
        File.WriteAllText(Path.Combine(destination, FileName), string.Join('\n', entries) + "\n");
}

/// <summary>Unpacks the source payload over an earlier release . The
/// payload's entries all carry one fixed timestamp (the ZIP is
/// deterministic), so a plain extraction stamped every source file with that
/// old date. The build tree left by the previous release is newer than that,
/// so Ninja saw nothing to rebuild and the update kept running the old
/// executable. Files whose bytes changed are written with the current time;
/// identical files are left alone, which keeps their build outputs valid.</summary>
public static class PayloadExtractor
{
    public static int Extract(string payload, string destination)
    {
        var root = Path.GetFullPath(destination).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var written = 0;
        using var archive = ZipFile.OpenRead(payload);
        foreach (var entry in archive.Entries)
        {
            var path = Path.GetFullPath(Path.Combine(root, entry.FullName));
            if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"The payload entry {entry.FullName} points outside the source folder.");
            if (string.IsNullOrEmpty(entry.Name))
            {
                Directory.CreateDirectory(path);
                continue;
            }
            if (SameContent(entry, path)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            entry.ExtractToFile(path, overwrite: true);
            File.SetLastWriteTimeUtc(path, DateTime.UtcNow);
            written++;
        }
        return written;
    }

    private static bool SameContent(ZipArchiveEntry entry, string path)
    {
        var info = new FileInfo(path);
        if (!info.Exists || info.Length != entry.Length) return false;
        using var existing = info.OpenRead();
        using var packed = entry.Open();
        var left = new byte[81920];
        var right = new byte[81920];
        while (true)
        {
            var read = existing.Read(left, 0, left.Length);
            var filled = 0;
            while (filled < read)
            {
                var got = packed.Read(right, filled, read - filled);
                if (got == 0) return false;
                filled += got;
            }
            if (read == 0) return true;
            if (!left.AsSpan(0, read).SequenceEqual(right.AsSpan(0, read))) return false;
        }
    }
}
