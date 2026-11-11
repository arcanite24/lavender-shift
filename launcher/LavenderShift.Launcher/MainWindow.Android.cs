using System.Diagnostics;
using System.IO;
using System.Text.RegularExpressions;
using System.Windows;
using LavenderShift.Launcher.Sharing;
using Microsoft.Win32;

namespace LavenderShift.Launcher;

// Android (docs/ANDROID.md), after Pinyon Shift's Android panel: the package
// is built from this PC's build (tools/android.ps1 doctor, then build, with
// the release's prebuilt Android shell), then installed over Wi-Fi with the
// share (QR code and pairing code) or, for a device with USB debugging on,
// over adb (install, push-data).
public partial class MainWindow
{
    private bool androidOpen;
    private AndroidShare? androidShare;
    private string? adbDevice;

    private string AndroidApk => Path.Combine(layout!.SourceRoot, ".local", "android", "lavender-shift.apk");

    private async void OpenAndroid_Click(object sender, RoutedEventArgs e)
    {
        if (layout is null || mode is not (Mode.Ready or Mode.Playing)) return;
        androidOpen = true;
        HideFailure();
        SetMode(mode);
        await DetectAdbDeviceAsync();
    }

    private void AndroidBack_Click(object sender, RoutedEventArgs e)
    {
        StopAndroidShare();
        androidOpen = false;
        AndroidStatusText.Text = "";
        SetMode(mode);
    }

    private void RefreshAndroidPanel()
    {
        if (layout is null) return;
        var built = File.Exists(AndroidApk);
        if (built)
        {
            var info = new FileInfo(AndroidApk);
            AndroidApkText.Text = $"{AndroidApk}\n{info.Length / (1024.0 * 1024.0):0} MB, built {info.LastWriteTime:g}. " +
                "Build again after an update or a new setup.";
        }
        else
        {
            AndroidApkText.Text = "Not built yet. The first build compiles the game for Android: 15 to 45 minutes and " +
                "about 15 GB of disk. It downloads the Android NDK and platform-tools from Google once (about 1 GB).";
        }
        var idle = mode != Mode.Running;
        AndroidBuildButton.Content = built ? "Build again" : "Build APK";
        AndroidBuildButton.IsEnabled = idle && androidShare is null;
        AndroidSaveButton.IsEnabled = built;
        AndroidShareButton.IsEnabled = idle && built;
        AndroidShareButton.Content = androidShare is null ? "Share on Wi-Fi" : "Stop sharing";
        AndroidUsbButton.Visibility = adbDevice is null ? Visibility.Collapsed : Visibility.Visible;
        AndroidUsbButton.IsEnabled = idle && built && androidShare is null;
        AndroidUsbButton.ToolTip = adbDevice is null ? null : $"Install the app and copy the game to {adbDevice} over USB with adb.";
    }

    private void AndroidSave_Click(object sender, RoutedEventArgs e)
    {
        if (layout is null || !File.Exists(AndroidApk)) return;
        var dialog = new SaveFileDialog
        {
            Title = "Save a copy of the Android package",
            FileName = "LavenderShift.apk",
            DefaultExt = "apk",
            Filter = "Android package (*.apk)|*.apk",
        };
        if (dialog.ShowDialog(this) != true) return;
        try
        {
            File.Copy(AndroidApk, dialog.FileName, overwrite: true);
            AndroidStatusText.Text = $"Saved {dialog.FileName}. The device also needs the game files: share them on Wi-Fi.";
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            AndroidStatusText.Text = "The copy failed: " + error.Message;
        }
    }

    private void AndroidShare_Click(object sender, RoutedEventArgs e)
    {
        if (layout is null) return;
        if (androidShare is not null)
        {
            StopAndroidShare();
            AndroidStatusText.Text = "Sharing stopped.";
            return;
        }
        if (!File.Exists(AndroidApk)) return;
        var addresses = AndroidShare.LocalAddresses();
        if (addresses.Count == 0)
        {
            AndroidStatusText.Text = "This PC is not on a local network. Connect it to the same Wi-Fi or router as the device.";
            return;
        }
        ShareContent content;
        try
        {
            content = AndroidShareContent.Build(layout.SourceRoot, layout.UserRoot);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            AndroidStatusText.Text = "The game files cannot be read: " + error.Message;
            return;
        }
        var share = new AndroidShare(AndroidApk, content.Groups, new { version = InstallLayout.Version });
        share.Activity += message => Dispatcher.BeginInvoke(() =>
        {
            AndroidStatusText.Text = message;
            AppendLog(message);
        });
        try { share.Start(); }
        catch (Exception error) when (error is System.Net.Sockets.SocketException or UnauthorizedAccessException
                                          or System.Net.HttpListenerException)
        {
            share.Dispose();
            AndroidStatusText.Text = "Sharing could not start: " + error.Message;
            return;
        }
        androidShare = share;
        var url = share.PageUrl(addresses[0]);
        AndroidQrImage.Source = QrCode.Encode(url).ToBitmapSource();
        AndroidCodeText.Text = share.Code[..3] + " " + share.Code[3..];
        AndroidUrlText.Text = $"Scan the code with the device's camera, or open {url} in its browser, install the app, " +
            "then open Lavender Shift on the device and enter the pairing code." +
            (addresses.Count > 1 ? $" This PC is also at {string.Join(", ", addresses.Skip(1))}." : "");
        AndroidSharePanel.Visibility = Visibility.Visible;
        var total = content.Groups.Sum(group => group.Bytes);
        AndroidStatusText.Text = $"Sharing {string.Join(", ", content.Groups.Select(group => group.Label))} " +
            $"({total / (1024.0 * 1024 * 1024):0.0} GB). Windows may ask to allow the launcher on private networks: allow it.";
        AppendLog($"Sharing the Android package and game files at {url}.");
        RefreshAndroidPanel();
    }

    private void StopAndroidShare()
    {
        androidShare?.Dispose();
        androidShare = null;
        AndroidSharePanel.Visibility = Visibility.Collapsed;
        AndroidQrImage.Source = null;
        if (androidOpen) RefreshAndroidPanel();
    }

    // adb from the SDK the build uses (doctor puts platform-tools in the
    // source's .local\toolchain\android-sdk when there is no SDK), or on PATH.
    private string? FindAdb()
    {
        var roots = new List<string?>
        {
            Environment.GetEnvironmentVariable("ANDROID_HOME"),
            Environment.GetEnvironmentVariable("ANDROID_SDK_ROOT"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Android", "Sdk"),
            layout is null ? null : Path.Combine(layout.SourceRoot, ".local", "toolchain", "android-sdk"),
        };
        var found = roots.Where(root => !string.IsNullOrWhiteSpace(root))
            .Select(root => Path.Combine(root!, "platform-tools", "adb.exe")).FirstOrDefault(File.Exists);
        return found ?? (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator)
            .Where(folder => folder.Length > 0).Select(folder => Path.Combine(folder, "adb.exe")).FirstOrDefault(File.Exists);
    }

    // A device with USB debugging on; the USB button stays hidden otherwise.
    private async Task DetectAdbDeviceAsync()
    {
        adbDevice = null;
        if (FindAdb() is { } adb)
        {
            try
            {
                var output = await Task.Run(() =>
                {
                    using var process = Process.Start(new ProcessStartInfo(adb, "devices")
                        { UseShellExecute = false, RedirectStandardOutput = true, CreateNoWindow = true })!;
                    var text = process.StandardOutput.ReadToEnd();
                    if (!process.WaitForExit(10000)) process.Kill();
                    return text;
                });
                adbDevice = output.Split('\n').Skip(1)
                    .Select(line => line.Trim().Split('\t'))
                    .Where(parts => parts.Length == 2 && parts[1] == "device")
                    .Select(parts => parts[0]).FirstOrDefault();
            }
            catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException) { }
        }
        if (androidOpen) RefreshAndroidPanel();
    }

    private async void AndroidBuild_Click(object sender, RoutedEventArgs e)
    {
        if (layout is null || mode == Mode.Running) return;
        if (GameIsRunning())
        {
            AndroidStatusText.Text = "Close Lavender Shift first: the Android build reads the PC build's files.";
            return;
        }
        // The player sees what will be installed and accepts the Android SDK
        // license before the NDK and platform-tools are downloaded.
        if (MessageBox.Show(this,
                "Build an Android package (APK) of the game from this PC's build, for your own device. " +
                "The first build takes 15 to 45 minutes and about 15 GB of disk.\n\n" +
                "It downloads the Android NDK and platform-tools from Google (about 1 GB, once), which are covered by " +
                "the Android Software Development Kit License Agreement (https://developer.android.com/studio/terms), " +
                "and the Mesa Turnip GPU driver (MIT license) it bundles for Adreno 7xx and 8xx GPUs.\n\n" +
                "Accept the Android SDK license and build?",
                "Build Android APK", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
            return;
        StopAndroidShare();
        var succeeded = await RunAndroidStepsAsync("Building for Android",
            "15 to 45 minutes the first time. You can leave it running; it continues where it stopped.",
            [
                (["doctor"], "Checking the Android tools.", 2, true),
                (["doctor", "--install", "--accept-licenses"],
                    "Downloading the Android NDK and platform-tools (about 1 GB, once).", 5, false),
                (["build", "--jobs", Environment.ProcessorCount.ToString()],
                    "Compiling the game for Android.", 15, false),
            ]);
        if (succeeded && !File.Exists(AndroidApk))
        {
            ShowFailure("The Android build finished without producing lavender-shift.apk.", null, null);
            succeeded = false;
        }
        FinishAndroidSteps(succeeded, "Android package ready",
            "Share on Wi-Fi to install it on the device, or save a copy.");
        await DetectAdbDeviceAsync();
    }

    private async void AndroidUsb_Click(object sender, RoutedEventArgs e)
    {
        if (layout is null || mode == Mode.Running || adbDevice is not { } device) return;
        StopAndroidShare();
        var succeeded = await RunAndroidStepsAsync("Installing over USB",
            "The game files are about 9 GB; an interrupted copy resumes.",
            [
                (["--serial", device, "install"], "Installing the app over USB.", 5, false),
                (["--serial", device, "push-data", "--user-root", layout.UserRoot],
                    "Copying the game files (about 9 GB; an interrupted copy resumes).", 20, false),
            ]);
        FinishAndroidSteps(succeeded, "Installed on the device",
            $"Installed on {device}. Start Lavender Shift from the device's app list.");
    }

    private void FinishAndroidSteps(bool succeeded, string headline, string status)
    {
        SetMode(Mode.Ready);
        if (succeeded)
        {
            HeadlineText.Text = headline;
            AndroidStatusText.Text = status;
        }
        else
        {
            HeadlineText.Text = cancelled ? "Stopped" : "Android build stopped";
            AndroidStatusText.Text = cancelled ? "Start it again to continue where it stopped." : "";
        }
    }

    // Runs tools/android.ps1 steps in order with the progress panel. A step
    // marked optional that succeeds skips the next one (doctor before its
    // --install). Ninja's [done/total] lines move the bar inside a step.
    private async Task<bool> RunAndroidStepsAsync(string headline, string subhead,
        IReadOnlyList<(string[] Arguments, string Message, int Percent, bool SkipNextOnSuccess)> androidSteps)
    {
        if (layout is null) return false;
        HideFailure();
        cancelled = false;
        log.Clear();
        LogTextBox.Text = "";
        SetMode(Mode.Running);
        HeadlineText.Text = headline;
        SetSubhead(subhead);
        var ninja = new Regex(@"^\[(\d+)/(\d+)\]");
        var environment = new Dictionary<string, string> { ["LAVENDER_SHIFT_VERSION"] = InstallLayout.Version };
        try
        {
            for (var index = 0; index < androidSteps.Count; index++)
            {
                var (arguments, message, start, skipNext) = androidSteps[index];
                var end = index + 1 < androidSteps.Count ? androidSteps[index + 1].Percent : 100;
                SetProgress(start, message);
                AppendLog(message);
                string? lastError = null;
                var tail = new List<string>();
                runner = new ScriptRunner();
                runner.Output += line => Dispatcher.BeginInvoke(() =>
                {
                    AppendLog(line);
                    tail.Add(line);
                    if (tail.Count > 8) tail.RemoveAt(0);
                    if (line.StartsWith("error: ", StringComparison.Ordinal)) lastError = line["error: ".Length..];
                    if (ninja.Match(line) is { Success: true } match &&
                        double.TryParse(match.Groups[2].Value, out var total) && total > 0)
                    {
                        var done = double.Parse(match.Groups[1].Value);
                        SetProgress((int)(start + (end - start) * done / total), message);
                    }
                });
                var code = await runner.RunAsync(layout.Tool("android.ps1"), arguments, layout.SourceRoot, environment);
                runner = null;
                if (cancelled) return false;
                if (code == 0 && skipNext) { index++; continue; }
                if (code != 0 && !skipNext)
                {
                    ShowFailure(lastError is null ? $"{message} failed (exit code {code})." : lastError + ".",
                        "Logs\android.log has the details; start it again to continue.", tail.ToList());
                    return false;
                }
            }
            SetProgress(100, "Done.");
            return true;
        }
        catch (Exception error)
        {
            runner = null;
            ShowFailure(error.Message, null, null);
            return false;
        }
        finally
        {
            try
            {
                Directory.CreateDirectory(layout.LogsRoot);
                File.WriteAllText(Path.Combine(layout.LogsRoot, "android.log"), log.ToString());
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        }
    }
}
