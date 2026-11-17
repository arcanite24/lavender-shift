using System.IO;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;

namespace LavenderShift.Launcher;

/// <summary>Save sync from the launcher (docs/SAVE_SYNC.md): the player picks
/// a folder a sync app (Syncthing, Dropbox, OneDrive...) shares between
/// devices; it goes in the game's settings file, and the game does the syncing.</summary>
public partial class MainWindow
{
    private void OpenSync_Click(object sender, RoutedEventArgs e)
    {
        var menu = new ContextMenu { PlacementTarget = OpenSyncButton };
        var choose = new MenuItem { Header = settings.SyncFolder is null ? "Choose a sync folder…" : "Choose another sync folder…" };
        choose.Click += (_, _) => ChooseSyncFolder();
        menu.Items.Add(choose);
        if (settings.SyncFolder is not null)
        {
            var open = new MenuItem { Header = "Open the sync folder" };
            open.Click += (_, _) => { if (Directory.Exists(settings.SyncFolder)) OpenUrl(settings.SyncFolder); };
            menu.Items.Add(open);
            var off = new MenuItem { Header = "Turn off save sync" };
            off.Click += (_, _) => SetSyncFolder(null);
            menu.Items.Add(off);
        }
        menu.IsOpen = true;
    }

    private void ChooseSyncFolder()
    {
        if (layout is null) return;
        var dialog = new OpenFolderDialog
        {
            Title = "Choose a folder your sync app shares between devices",
            InitialDirectory = settings.SyncFolder ?? Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
        };
        if (dialog.ShowDialog(this) != true) return;
        var folder = Path.GetFullPath(dialog.FolderName);
        // An update or a removed install would take the folder with it, and
        // the user folder is the live save the sync folder must stay apart from.
        if (IsInside(folder, layout.InstallRoot) || IsInside(folder, layout.UserRoot))
        {
            MessageBox.Show(this, "Choose a folder outside the Lavender Shift folder, such as one your sync app already shares.",
                "Save sync", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }
        SetSyncFolder(folder);
    }

    private void SetSyncFolder(string? folder)
    {
        if (layout is null) return;
        settings.SyncFolder = folder;
        settings.Save(layout.SettingsPath);
        // The game reads it from its settings file however it is started.
        GameStarter.WriteSyncSetting(layout, folder);
        OpenSyncButton.Content = folder is null ? "Save sync" : "Save sync: on";
        if (mode == Mode.Ready && !editingFiles) SetSubhead(ReadySubhead());
    }

    /// <summary>The ready line, with the save sync status when it is on.</summary>
    private string ReadySubhead()
    {
        const string ready = "Vulkan · F6 opens settings in game";
        if (layout is null || settings.SyncFolder is null) return ready;
        return ready + " · " + SaveSyncStatus.Describe(settings.SyncFolder, layout.UserRoot);
    }

    private static bool IsInside(string path, string folder)
    {
        var root = Path.GetFullPath(folder).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        return (Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar)
            .StartsWith(root, StringComparison.OrdinalIgnoreCase);
    }
}
