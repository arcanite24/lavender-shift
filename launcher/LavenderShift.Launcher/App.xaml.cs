using System.Windows;
using System.Windows.Input;

namespace LavenderShift.Launcher;

public partial class App : Application
{
    // Once the game is built, opening the launcher plays: the launcher window
    // shows only for setup, an update, a failed start, or when the player
    // holds Shift (or passes --launcher) to reach save sync, Android or the files.
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        var wantLauncher = e.Args.Any(arg => string.Equals(arg, "--launcher", StringComparison.OrdinalIgnoreCase)) ||
            (Keyboard.Modifiers & ModifierKeys.Shift) != 0;
        if (!wantLauncher && GameStarter.TryQuickStart())
        {
            Shutdown();
            return;
        }
        new MainWindow().Show();
    }
}
