using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows.Media;

namespace LavenderShift.Launcher;

public enum StepState { Waiting, Active, Complete, Failed }

/// <summary>One bar of the setup route under the action row (as in Pinyon
/// Shift's launcher). Setup's event stages map onto these.</summary>
public sealed class RouteStep(string stage, string title) : INotifyPropertyChanged
{
    public static readonly Brush WaitingBrush = Frozen(0x42, 0x37, 0x4F);
    public static readonly Brush ActiveBrush = Frozen(0xF1, 0xAE, 0x36);
    public static readonly Brush CompleteBrush = Frozen(0xB5, 0x9A, 0xF0);
    public static readonly Brush FailedBrush = Frozen(0xE1, 0x6E, 0x5F);

    private Brush brush = WaitingBrush;

    public string Stage { get; } = stage;
    public string Title { get; } = title;
    public StepState State { get; private set; }
    public Brush Brush { get => brush; private set { brush = value; Notify(); } }

    public event PropertyChangedEventHandler? PropertyChanged;

    public void SetState(StepState state)
    {
        State = state;
        Brush = state switch
        {
            StepState.Active => ActiveBrush,
            StepState.Complete => CompleteBrush,
            StepState.Failed => FailedBrush,
            _ => WaitingBrush,
        };
    }

    private static Brush Frozen(byte r, byte g, byte b)
    {
        var value = new SolidColorBrush(Color.FromRgb(r, g, b));
        value.Freeze();
        return value;
    }

    private void Notify([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
