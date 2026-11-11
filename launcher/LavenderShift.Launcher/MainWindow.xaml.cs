using System.Diagnostics;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Win32;

namespace LavenderShift.Launcher;

public partial class MainWindow : Window
{
    private enum Mode { NeedsSetup, Running, Ready, Playing }

    private const int LogLimit = 400_000;
    private readonly InstallLayout? layout;
    private readonly LauncherSettings settings = new();
    private readonly DispatcherTimer gameWatch = new() { Interval = TimeSpan.FromSeconds(2) };
    private readonly StringBuilder log = new();
    private ScriptRunner? runner;
    private Mode mode;
    private bool editingFiles;
    private bool cancelled;

    public MainWindow()
    {
        InitializeComponent();
        VersionText.Text = "v" + InstallLayout.Version;
        gameWatch.Tick += (_, _) => { if (mode == Mode.Playing && !GameIsRunning()) SetMode(Mode.Ready); };
        try
        {
            layout = InstallLayout.Locate();
            settings = LauncherSettings.Load(layout.SettingsPath);
        }
        catch (Exception ex)
        {
            ShowFailure(ex.Message, null, null);
            PrimaryButton.Content = "Set up";
            PrimaryButton.IsEnabled = false;
            SetupPanel.IsEnabled = false;
            RepairButton.Visibility = ChangeFilesButton.Visibility = Visibility.Collapsed;
            return;
        }
        DiscBox.Text = settings.DiscImage ?? "";
        UpdateBox.Text = settings.TitleUpdate ?? "";
        OwnershipBox.IsChecked = settings.OwnershipConfirmed;
        SetupNote.Text =
            $"Setup downloads the pinned build tools (and the Visual Studio Build Tools if this PC has none, which asks for " +
            $"administrator rights), extracts your files and builds the game in {layout.SourceRoot}. The first setup takes " +
            "about an hour and needs 30 GB free there. Your files are only read, and nothing from them is uploaded.";
        if (layout.InstallRoot.Length > 90)
            SetupNote.Text += " This folder's path is long; if the build fails, move the launcher folder somewhere short, such as C:\\Games\\LavenderShift.";
        SetMode(GameIsRunning() ? Mode.Playing : layout.IsReady() ? Mode.Ready : Mode.NeedsSetup);
        var failure = mode == Mode.NeedsSetup ? SetupFailure.Read(layout.SetupErrorPath) : null;
        if (failure is not null) ShowFailure("The last setup failed: " + failure.Message, failure.Hint, failure.Excerpt);
        if (mode == Mode.NeedsSetup && layout.IsReadyExceptSource())
            DetailText.Text = "This release changed the game's source. Run setup again to rebuild (your game files are kept).";
    }

    private void SetMode(Mode next)
    {
        mode = next;
        var running = next == Mode.Running;
        SetupPanel.Visibility = next == Mode.NeedsSetup || editingFiles || (running && editingFiles)
            ? Visibility.Visible : Visibility.Collapsed;
        SetupPanel.IsEnabled = !running;
        Progress.Visibility = running ? Visibility.Visible : Visibility.Collapsed;
        RepairButton.Visibility = next is Mode.Ready && !editingFiles ? Visibility.Visible : Visibility.Collapsed;
        ChangeFilesButton.Visibility = next is Mode.Ready && !editingFiles ? Visibility.Visible : Visibility.Collapsed;
        switch (next)
        {
            case Mode.NeedsSetup:
                StatusText.Text = "Setup needed";
                PrimaryButton.Content = "Set up";
                break;
            case Mode.Running:
                StatusText.Text = "Setting up";
                PrimaryButton.Content = "Cancel";
                break;
            case Mode.Ready:
                StatusText.Text = editingFiles ? "Choose new files, then run setup" : "Ready to play";
                PrimaryButton.Content = editingFiles ? "Set up" : "Play";
                if (!editingFiles) DetailText.Text = $"Saves and settings: {layout!.UserRoot}";
                gameWatch.Stop();
                break;
            case Mode.Playing:
                StatusText.Text = "Lavender Shift is running";
                DetailText.Text = "Settings are in the game's F6 menu. Close the game to run setup again.";
                PrimaryButton.Content = "Play";
                gameWatch.Start();
                break;
        }
        UpdatePrimaryEnabled();
    }

    private void UpdatePrimaryEnabled()
    {
        if (layout is null) return;
        PrimaryButton.IsEnabled = mode switch
        {
            Mode.Running => true,
            Mode.Playing => false,
            Mode.Ready when !editingFiles => true,
            _ => File.Exists(DiscBox.Text.Trim()) && File.Exists(UpdateBox.Text.Trim()) && OwnershipBox.IsChecked == true,
        };
    }

    private void Inputs_Changed(object sender, RoutedEventArgs e) => UpdatePrimaryEnabled();

    private void BrowseDisc_Click(object sender, RoutedEventArgs e) =>
        Browse(DiscBox, "Disc images (*.iso)|*.iso|All files (*.*)|*.*");

    private void BrowseUpdate_Click(object sender, RoutedEventArgs e) =>
        Browse(UpdateBox, "Title update (*.zip;*.*)|*.*");

    private void Browse(System.Windows.Controls.TextBox box, string filter)
    {
        var dialog = new OpenFileDialog { Filter = filter, CheckFileExists = true };
        var current = box.Text.Trim();
        if (File.Exists(current)) dialog.InitialDirectory = Path.GetDirectoryName(current);
        if (dialog.ShowDialog(this) == true) box.Text = dialog.FileName;
    }

    private async void PrimaryButton_Click(object sender, RoutedEventArgs e)
    {
        switch (mode)
        {
            case Mode.Running:
                cancelled = true;
                runner?.Cancel();
                break;
            case Mode.Ready when !editingFiles:
                await PlayAsync();
                break;
            default:
                await RunSetupAsync();
                break;
        }
    }

    private async void RepairButton_Click(object sender, RoutedEventArgs e) => await RunSetupAsync();

    private void ChangeFilesButton_Click(object sender, RoutedEventArgs e)
    {
        editingFiles = true;
        SetMode(Mode.Ready);
    }

    private async Task RunSetupAsync()
    {
        if (layout is null) return;
        if (GameIsRunning())
        {
            ShowFailure("Close Lavender Shift before setup. Windows locks the game files while it runs.", null, null);
            return;
        }
        if (!File.Exists(DiscBox.Text.Trim()) || !File.Exists(UpdateBox.Text.Trim()))
        {
            editingFiles = true;
            SetMode(layout.IsReady() ? Mode.Ready : Mode.NeedsSetup);
            ShowFailure("Choose your disc image and title update first.", null, null);
            return;
        }
        settings.DiscImage = DiscBox.Text.Trim();
        settings.TitleUpdate = UpdateBox.Text.Trim();
        settings.OwnershipConfirmed = OwnershipBox.IsChecked == true;
        settings.Save(layout.SettingsPath);

        HideFailure();
        cancelled = false;
        SetMode(Mode.Running);
        Progress.Value = 0;
        DetailText.Text = "Starting setup.";
        AppendLog($"Setup started {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
        try
        {
            if (!layout.SourceIsCurrent())
            {
                DetailText.Text = "Unpacking the source.";
                var written = await Task.Run(layout.ExtractPayload);
                AppendLog($"Unpacked the source into {layout.SourceRoot} ({written} files changed).");
            }
            runner = new ScriptRunner();
            runner.Output += line => Dispatcher.BeginInvoke(() => AppendLog(line));
            runner.Progress += progress => Dispatcher.BeginInvoke(() =>
            {
                Progress.Value = progress.Percent;
                DetailText.Text = progress.Message;
                AppendLog($"[{progress.Stage}] {progress.Message}");
            });
            Directory.CreateDirectory(layout.UserRoot);
            var code = await runner.RunAsync(layout.Tool("setup-release.ps1"),
                ["-IsoPath", settings.DiscImage, "-UpdatePath", settings.TitleUpdate, "-UserRoot", layout.UserRoot, "-JsonEvents"],
                layout.SourceRoot);
            runner = null;
            if (code == 0 && layout.IsReady())
            {
                editingFiles = false;
                SetMode(Mode.Ready);
                StatusText.Text = "Ready to play";
                return;
            }
            SetMode(Mode.NeedsSetup);
            if (cancelled)
            {
                StatusText.Text = "Setup stopped";
                DetailText.Text = "Setup continues where it stopped when you start it again.";
                return;
            }
            var failure = SetupFailure.Read(layout.SetupErrorPath);
            ShowFailure(failure?.Message ?? $"Setup failed (exit code {code}). The setup log below has the details.",
                failure?.Hint, failure?.Excerpt);
            LogExpander.IsExpanded = true;
        }
        catch (Exception ex)
        {
            runner = null;
            SetMode(Mode.NeedsSetup);
            ShowFailure(ex.Message, null, null);
        }
    }

    private async Task PlayAsync()
    {
        if (layout is null) return;
        HideFailure();
        PrimaryButton.IsEnabled = false;
        DetailText.Text = "Starting the game.";
        var output = new StringBuilder();
        var launch = new ScriptRunner();
        launch.Output += line => { lock (output) output.AppendLine(line); };
        int code;
        try
        {
            code = await launch.RunAsync(layout.Tool("launch.ps1"), ["-UserRoot", layout.UserRoot], layout.SourceRoot);
        }
        catch (Exception ex)
        {
            ShowFailure("The game could not be started: " + ex.Message, null, null);
            SetMode(Mode.Ready);
            return;
        }
        AppendLog(output.ToString().TrimEnd());
        if (code != 0)
        {
            var lines = output.ToString().Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            ShowFailure("The game could not be started.", "Run setup again if this keeps happening.", lines.TakeLast(6).ToList());
            SetMode(Mode.Ready);
            return;
        }
        SetMode(Mode.Playing);
    }

    private static bool GameIsRunning()
    {
        var processes = Process.GetProcessesByName("lavender_shift");
        foreach (var process in processes) process.Dispose();
        return processes.Length > 0;
    }

    private void ShowFailure(string message, string? hint, IReadOnlyList<string>? excerpt)
    {
        FailurePanel.Visibility = Visibility.Visible;
        FailureText.Text = message;
        FailureHint.Text = hint is null ? "" : "What to try: " + hint;
        FailureHint.Visibility = hint is null ? Visibility.Collapsed : Visibility.Visible;
        FailureExcerpt.Text = excerpt is null ? "" : string.Join(Environment.NewLine, excerpt);
        FailureExcerpt.Visibility = excerpt is { Count: > 0 } ? Visibility.Visible : Visibility.Collapsed;
    }

    private void HideFailure() => FailurePanel.Visibility = Visibility.Collapsed;

    private void AppendLog(string line)
    {
        if (line.Length == 0) return;
        log.AppendLine(line);
        if (log.Length > LogLimit) log.Remove(0, log.Length - LogLimit / 2);
        LogBox.Text = log.ToString();
        LogBox.ScrollToEnd();
    }

    private void OpenSaves_Click(object sender, RoutedEventArgs e)
    {
        if (layout is null) return;
        Directory.CreateDirectory(layout.UserRoot);
        OpenFolder(layout.UserRoot);
    }

    private void OpenLogs_Click(object sender, RoutedEventArgs e)
    {
        if (layout is null) return;
        Directory.CreateDirectory(layout.LogsRoot);
        OpenFolder(layout.LogsRoot);
    }

    private static void OpenFolder(string path) =>
        Process.Start(new ProcessStartInfo { FileName = path, UseShellExecute = true })?.Dispose();

    protected override void OnClosing(System.ComponentModel.CancelEventArgs e)
    {
        if (mode == Mode.Running &&
            MessageBox.Show(this, "Setup is still running. Stop it? It continues where it stopped next time.",
                "Lavender Shift", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
        {
            e.Cancel = true;
            return;
        }
        cancelled = true;
        runner?.Cancel();
        base.OnClosing(e);
    }
}
