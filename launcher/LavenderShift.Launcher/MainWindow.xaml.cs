using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;

namespace LavenderShift.Launcher;

public partial class MainWindow : Window
{
    private enum Mode { NeedsSetup, Running, Ready, Playing }

    private const int LogLimit = 400_000;

    // Setup's event stages (tools/setup-release.ps1) in order; shader
    // preparation belongs to Play, as in Pinyon Shift's launcher.
    private readonly ObservableCollection<RouteStep> steps =
    [
        new("tools", "Get build tools"),
        new("extract", "Extract game"),
        new("build", "Build"),
        new("play", "Play"),
    ];

    private readonly InstallLayout? layout;
    private readonly LauncherSettings settings = new();
    private readonly DispatcherTimer gameWatch = new() { Interval = TimeSpan.FromSeconds(2) };
    private readonly StringBuilder log = new();
    private ScriptRunner? runner;
    private Mode mode;
    private bool editingFiles;
    private bool cancelled;
    private string? discPath;
    private string? updatePath;

    public MainWindow()
    {
        InitializeComponent();
        RouteList.ItemsSource = steps;
        VersionRun.Text = "Lavender Shift " + InstallLayout.Version;
        gameWatch.Tick += (_, _) => { if (mode == Mode.Playing && !GameIsRunning()) SetMode(Mode.Ready); };
        StateChanged += (_, _) =>
        {
            // A maximized borderless window reaches past the screen edge by the
            // resize border; pad it back in.
            RootBorder.Margin = WindowState == WindowState.Maximized ? new Thickness(7) : new Thickness(0);
            MaximizeGlyph.Data = Geometry.Parse(WindowState == WindowState.Maximized
                ? "M2.5,0.5 H9.5 V7.5 M0.5,2.5 H7.5 V9.5 H0.5 Z"
                : "M0.5,0.5 H9.5 V9.5 H0.5 Z");
        };
        try
        {
            layout = InstallLayout.Locate();
            settings = LauncherSettings.Load(layout.SettingsPath);
        }
        catch (Exception ex)
        {
            HeadlineText.Text = "Something is missing";
            SetSubhead("Extract the whole release ZIP into one folder and start the launcher from there.");
            ShowFailure(ex.Message, null, null);
            SetupPanel.Visibility = Visibility.Collapsed;
            RouteList.Visibility = Visibility.Collapsed;
            PrimaryButton.IsEnabled = false;
            return;
        }
        SetDisc(settings.DiscImage);
        SetUpdate(settings.TitleUpdate);
        OwnershipCheckBox.IsChecked = settings.OwnershipConfirmed;
        BuildLocationRun.Text = layout.SourceRoot;
        if (layout.InstallRoot.Length > 90)
            BuildLocationNoteRun.Text = " · needs 30 GB free; a shorter folder such as C:\\Games\\LavenderShift avoids path-length build errors";
        SetMode(GameIsRunning() ? Mode.Playing : layout.IsReady() ? Mode.Ready : Mode.NeedsSetup);
        if (mode == Mode.NeedsSetup)
        {
            var failure = SetupFailure.Read(layout.SetupErrorPath);
            if (failure is not null)
            {
                HeadlineText.Text = "Setup stopped";
                ShowFailure(failure.Message, failure.Hint, failure.Excerpt);
            }
            if (layout.IsReadyExceptSource())
            {
                HeadlineText.Text = "Update ready";
                SetSubhead("This release changed the game's source. Set up again to rebuild; your game files and saves are kept.");
            }
        }
    }

    private void SetMode(Mode next)
    {
        mode = next;
        var running = next == Mode.Running;
        var ready = next is Mode.Ready or Mode.Playing;
        SetupPanel.Visibility = !running && (next == Mode.NeedsSetup || editingFiles) ? Visibility.Visible : Visibility.Collapsed;
        LogPanel.Visibility = running ? Visibility.Visible : Visibility.Collapsed;
        RouteList.Visibility = ready && !editingFiles ? Visibility.Collapsed : Visibility.Visible;
        RepairButton.Visibility = next == Mode.Ready && !editingFiles ? Visibility.Visible : Visibility.Collapsed;
        ChangeFilesButton.Visibility = next == Mode.Ready && !editingFiles ? Visibility.Visible : Visibility.Collapsed;
        PrimaryIcon.Visibility = next == Mode.Ready && !editingFiles ? Visibility.Visible : Visibility.Collapsed;
        switch (next)
        {
            case Mode.NeedsSetup:
                HeadlineText.Text = "Set up Lavender Shift";
                SetSubhead("Built on this PC from your own disc and title update. Nothing is uploaded.");
                PrimaryButtonText.Text = "Set up";
                break;
            case Mode.Running:
                HeadlineText.Text = "Setting up";
                SetSubhead("15 minutes to an hour, depending on the PC. If it stops, it continues where it left off.");
                PrimaryButtonText.Text = "Cancel";
                break;
            case Mode.Ready when editingFiles:
                HeadlineText.Text = "Choose your files";
                SetSubhead("Setup checks them and rebuilds what changed. Your saves are kept.");
                PrimaryButtonText.Text = "Set up";
                gameWatch.Stop();
                break;
            case Mode.Ready:
                HeadlineText.Text = "Ready to drive";
                SetSubhead("Vulkan · F6 opens settings in game");
                PrimaryButtonText.Text = "Play";
                foreach (var step in steps) step.SetState(StepState.Complete);
                gameWatch.Stop();
                break;
            case Mode.Playing:
                HeadlineText.Text = "Game running";
                SetSubhead("Controller A or Enter to start. F6 opens settings. Close the game to run setup again.");
                PrimaryButtonText.Text = "Game running";
                gameWatch.Start();
                break;
        }
        UpdatePrimaryEnabled();
    }

    private void SetSubhead(string text) => SubheadText.Text = text;

    private void UpdatePrimaryEnabled()
    {
        if (layout is null) return;
        PrimaryButton.IsEnabled = mode switch
        {
            Mode.Running => true,
            Mode.Playing => false,
            Mode.Ready when !editingFiles => true,
            _ => File.Exists(discPath) && File.Exists(updatePath) && OwnershipCheckBox.IsChecked == true,
        };
    }

    private void InputChanged(object sender, RoutedEventArgs e) => UpdatePrimaryEnabled();

    private void SetDisc(string? path)
    {
        discPath = string.IsNullOrWhiteSpace(path) ? null : path;
        if (discPath is null)
        {
            DiscTitleText.Text = "Drop your Forza Horizon 2 disc image";
            DiscDetailText.Text = "The USA disc, dumped from your own copy.";
            BrowseDiscButton.Content = "Choose ISO";
        }
        else
        {
            DiscTitleText.Text = Path.GetFileName(discPath);
            DiscDetailText.Text = File.Exists(discPath) ? Path.GetDirectoryName(discPath) : "Not found: " + discPath;
            BrowseDiscButton.Content = "Change";
        }
        UpdatePrimaryEnabled();
    }

    private void SetUpdate(string? path)
    {
        updatePath = string.IsNullOrWhiteSpace(path) ? null : path;
        if (updatePath is null)
        {
            UpdateTitleText.Text = "And the title update v2";
            UpdateDetailText.Text = "The update package from your console, or a ZIP holding it.";
            BrowseUpdateButton.Content = "Choose update";
        }
        else
        {
            UpdateTitleText.Text = Path.GetFileName(updatePath);
            UpdateDetailText.Text = File.Exists(updatePath) ? Path.GetDirectoryName(updatePath) : "Not found: " + updatePath;
            BrowseUpdateButton.Content = "Change";
        }
        UpdatePrimaryEnabled();
    }

    private void BrowseDisc_Click(object sender, RoutedEventArgs e) =>
        SetDisc(Browse(discPath, "Disc images (*.iso)|*.iso|All files (*.*)|*.*") ?? discPath);

    private void BrowseUpdate_Click(object sender, RoutedEventArgs e) =>
        SetUpdate(Browse(updatePath, "Title update (*.zip, package)|*.*") ?? updatePath);

    private string? Browse(string? current, string filter)
    {
        var dialog = new OpenFileDialog { Filter = filter, CheckFileExists = true };
        if (File.Exists(current)) dialog.InitialDirectory = Path.GetDirectoryName(current);
        return dialog.ShowDialog(this) == true ? dialog.FileName : null;
    }

    // A disc image is an .iso; any other dropped file is taken as the update.
    private static bool TryGetDrop(DragEventArgs e, out string path)
    {
        path = "";
        if (e.Data.GetData(DataFormats.FileDrop) is not string[] { Length: 1 } files || !File.Exists(files[0])) return false;
        path = files[0];
        return true;
    }

    private static bool IsDiscImage(string path) =>
        Path.GetExtension(path).Equals(".iso", StringComparison.OrdinalIgnoreCase);

    private void Window_DragOver(object sender, DragEventArgs e)
    {
        var path = "";
        var accepted = layout is not null && mode != Mode.Running && TryGetDrop(e, out path);
        if (accepted)
            DropOverlayText.Text = IsDiscImage(path) ? "Drop to use this disc image" : "Drop to use this title update";
        e.Effects = accepted ? DragDropEffects.Copy : DragDropEffects.None;
        DropOverlay.Visibility = accepted ? Visibility.Visible : Visibility.Collapsed;
        e.Handled = true;
    }

    protected override void OnDragLeave(DragEventArgs e)
    {
        base.OnDragLeave(e);
        DropOverlay.Visibility = Visibility.Collapsed;
    }

    private void Window_Drop(object sender, DragEventArgs e)
    {
        DropOverlay.Visibility = Visibility.Collapsed;
        e.Handled = true;
        if (layout is null || mode == Mode.Running || !TryGetDrop(e, out var path)) return;
        if (mode is Mode.Ready && !editingFiles)
        {
            editingFiles = true;
            SetMode(Mode.Ready);
        }
        if (IsDiscImage(path)) SetDisc(path);
        else SetUpdate(path);
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

    private void MicrosoftLicenseLink_Click(object sender, RoutedEventArgs e) =>
        OpenUrl("https://go.microsoft.com/fwlink/?LinkId=2179911");

    private async Task RunSetupAsync()
    {
        if (layout is null) return;
        if (GameIsRunning())
        {
            ShowFailure("Close Lavender Shift before setup. Windows locks the game files while it runs.", null, null);
            return;
        }
        if (!File.Exists(discPath) || !File.Exists(updatePath))
        {
            editingFiles = true;
            SetMode(layout.IsReady() ? Mode.Ready : Mode.NeedsSetup);
            ShowFailure("Choose your disc image and title update first.", null, null);
            return;
        }
        settings.DiscImage = discPath;
        settings.TitleUpdate = updatePath;
        settings.OwnershipConfirmed = OwnershipCheckBox.IsChecked == true;
        settings.Save(layout.SettingsPath);

        HideFailure();
        cancelled = false;
        foreach (var step in steps) step.SetState(StepState.Waiting);
        steps[0].SetState(StepState.Active);
        SetMode(Mode.Running);
        SetProgress(0, "Starting setup.");
        AppendLog($"Setup started {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
        try
        {
            if (!layout.SourceIsCurrent())
            {
                SetProgress(1, "Unpacking the source.");
                var written = await Task.Run(layout.ExtractPayload);
                AppendLog($"Unpacked the source into {layout.SourceRoot} ({written} files changed).");
            }
            runner = new ScriptRunner();
            runner.Output += line => Dispatcher.BeginInvoke(() => AppendLog(line));
            runner.Progress += progress => Dispatcher.BeginInvoke(() => HandleProgress(progress));
            Directory.CreateDirectory(layout.UserRoot);
            var code = await runner.RunAsync(layout.Tool("setup-release.ps1"),
                ["-IsoPath", discPath!, "-UpdatePath", updatePath!, "-UserRoot", layout.UserRoot, "-JsonEvents"],
                layout.SourceRoot);
            runner = null;
            if (code == 0 && layout.IsReady())
            {
                editingFiles = false;
                SetMode(Mode.Ready);
                return;
            }
            MarkActiveStepFailed();
            SetMode(Mode.NeedsSetup);
            HeadlineText.Text = "Setup stopped";
            if (cancelled)
            {
                SetSubhead("It continues where it stopped when you set up again.");
                return;
            }
            var failure = SetupFailure.Read(layout.SetupErrorPath);
            ShowFailure(failure?.Message ?? $"Setup failed (exit code {code}). The log in the Logs folder has the details.",
                failure?.Hint, failure?.Excerpt);
        }
        catch (Exception ex)
        {
            runner = null;
            MarkActiveStepFailed();
            SetMode(Mode.NeedsSetup);
            HeadlineText.Text = "Setup stopped";
            ShowFailure(ex.Message, null, null);
        }
    }

    private void HandleProgress(ProgressEvent progress)
    {
        var stage = progress.Stage == "shaders" ? "play" : progress.Stage;
        var index = steps.ToList().FindIndex(step => step.Stage == stage);
        if (index >= 0)
            for (var i = 0; i < steps.Count; i++)
                steps[i].SetState(i < index ? StepState.Complete : i == index ? StepState.Active : StepState.Waiting);
        if (progress.Stage == "shaders") HeadlineText.Text = "Preparing graphics";
        SetProgress(progress.Percent, progress.Message);
        AppendLog($"[{progress.Stage}] {progress.Message}");
    }

    private void MarkActiveStepFailed()
    {
        foreach (var step in steps.Where(step => step.State == StepState.Active)) step.SetState(StepState.Failed);
    }

    private void SetProgress(int percent, string message)
    {
        BuildProgress.Value = percent;
        ProgressText.Text = $"{percent}%";
        ProgressMessageText.Text = message;
    }

    private async Task PlayAsync()
    {
        if (layout is null) return;
        HideFailure();
        PrimaryButton.IsEnabled = false;
        PrimaryButtonText.Text = "Starting…";
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
            SetMode(Mode.Ready);
            ShowFailure("The game could not be started: " + ex.Message, null, null);
            return;
        }
        AppendLog(output.ToString().TrimEnd());
        if (code != 0)
        {
            var lines = output.ToString().Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            SetMode(Mode.Ready);
            ShowFailure("The game could not be started.", "Run setup again if this keeps happening.", lines.TakeLast(6).ToList());
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
        LogTextBox.Text = log.ToString();
        LogTextBox.ScrollToEnd();
    }

    private void LogToggleButton_Click(object sender, RoutedEventArgs e)
    {
        var show = LogBox.Visibility != Visibility.Visible;
        LogBox.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        LogToggleButton.Content = show ? "Hide details" : "Show details";
    }

    private void OpenSaves_Click(object sender, RoutedEventArgs e)
    {
        if (layout is null) return;
        Directory.CreateDirectory(layout.UserRoot);
        OpenUrl(layout.UserRoot);
    }

    private void OpenLogs_Click(object sender, RoutedEventArgs e)
    {
        if (layout is null) return;
        Directory.CreateDirectory(layout.LogsRoot);
        OpenUrl(layout.LogsRoot);
    }

    private static void OpenUrl(string target) =>
        Process.Start(new ProcessStartInfo { FileName = target, UseShellExecute = true })?.Dispose();

    private void MinimizeButton_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    private void MaximizeButton_Click(object sender, RoutedEventArgs e) =>
        WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

    private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();

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
