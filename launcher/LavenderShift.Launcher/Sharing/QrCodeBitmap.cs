using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace LavenderShift.Launcher.Sharing;

// The QR code (QrCode.cs) as a WPF bitmap, for the Android panel's download
// link: one pixel per module with a four-module quiet zone. Show it in an
// Image with RenderOptions.BitmapScalingMode="NearestNeighbor" so the
// modules stay sharp when scaled.
public static class QrCodeBitmap
{
    public static BitmapSource ToBitmapSource(this QrCode code)
    {
        var image = code.ToGray8();
        var bitmap = BitmapSource.Create(image.Side, image.Side, 96, 96, PixelFormats.Gray8, null,
            image.Pixels, image.Side);
        bitmap.Freeze();
        return bitmap;
    }
}
