using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;

namespace LavenderShift.Launcher;

public sealed record ProgressEvent(string Stage, int Percent, string Message);

/// <summary>Runs a tools\*.ps1 script under Windows PowerShell with its output
/// redirected. Lines that start with '::lavender::' are progress events
/// (tools/release-common.ps1, Write-LavenderEvent); every other line is log
/// text.</summary>
public sealed class ScriptRunner
{
    private const string EventPrefix = "::lavender::";
    private Process? process;

    public event Action<ProgressEvent>? Progress;
    public event Action<string>? Output;

    public async Task<int> RunAsync(string script, IEnumerable<string> arguments, string workingDirectory)
    {
        var info = new ProcessStartInfo
        {
            FileName = Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe"),
            WorkingDirectory = workingDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (var argument in new[] { "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script })
            info.ArgumentList.Add(argument);
        foreach (var argument in arguments) info.ArgumentList.Add(argument);

        process = new Process { StartInfo = info, EnableRaisingEvents = true };
        process.OutputDataReceived += (_, e) => { if (e.Data is not null) HandleLine(e.Data); };
        process.ErrorDataReceived += (_, e) => { if (e.Data is not null) Output?.Invoke(e.Data); };
        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        await process.WaitForExitAsync();
        var code = process.ExitCode;
        process.Dispose();
        process = null;
        return code;
    }

    /// <summary>Stops the script and everything it started (CMake, Ninja,
    /// the compiler). Setup continues where it stopped next time.</summary>
    public void Cancel()
    {
        try { process?.Kill(entireProcessTree: true); }
        catch (InvalidOperationException) { }
    }

    private void HandleLine(string line)
    {
        var start = line.IndexOf(EventPrefix, StringComparison.Ordinal);
        if (start >= 0)
        {
            try
            {
                using var json = JsonDocument.Parse(line[(start + EventPrefix.Length)..]);
                var root = json.RootElement;
                Progress?.Invoke(new ProgressEvent(
                    root.GetProperty("stage").GetString() ?? "",
                    Math.Clamp(root.GetProperty("percent").GetInt32(), 0, 100),
                    root.GetProperty("message").GetString() ?? ""));
                return;
            }
            catch (Exception ex) when (ex is JsonException or KeyNotFoundException or InvalidOperationException) { }
        }
        Output?.Invoke(line);
    }
}

/// <summary>The part of .local\logs\setup-error.json the launcher shows.</summary>
public sealed record SetupFailure(string Message, string? Step, string? Hint, IReadOnlyList<string> Excerpt)
{
    public static SetupFailure? Read(string path)
    {
        try
        {
            if (!File.Exists(path)) return null;
            using var json = JsonDocument.Parse(File.ReadAllText(path));
            var root = json.RootElement;
            string? Text(string name) =>
                root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;
            var excerpt = new List<string>();
            if (root.TryGetProperty("error_excerpt", out var lines) && lines.ValueKind == JsonValueKind.Array)
                excerpt.AddRange(lines.EnumerateArray().Select(line => line.ToString()));
            else if (root.TryGetProperty("error_excerpt", out var single) && single.ValueKind == JsonValueKind.String)
                excerpt.Add(single.GetString()!);
            return new SetupFailure(Text("message") ?? "Setup failed.", Text("step"), Text("hint"), excerpt);
        }
        catch (Exception ex) when (ex is JsonException or IOException) { return null; }
    }
}
