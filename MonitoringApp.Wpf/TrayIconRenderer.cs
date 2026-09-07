using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;

namespace MonitoringApp;

/// <summary>
/// 托盘图标渲染 v7（圆形仪表盘风，参考腾讯管家样式重构）。
/// 单指标：彩色圆盘（上亮下暗渐变）+ 同色系深色内环 + 纯白粗体数字居中；
/// 网速：优先走 NetSpeedWidget 任务栏文本小组件，方形网速图标仅作兜底（双胶囊样式保留）。
/// 全部按图标真实像素 4x 超采样绘制，再高质量降采样，浅色 / 深色任务栏均清晰锐利。
/// </summary>
internal static class TrayIconRenderer
{
    /// <summary>托盘数字字体：窄面粗体，四位数字也能尽量大。</summary>
    private const string FontFamily = "Bahnschrift Condensed";

    /// <summary>超采样倍数：先按 4x 尺寸绘制，再降采样到目标尺寸。</summary>
    private const int Supersample = 4;

    // —— 徽章底色（色相延续旧版身份色：功耗橙 / 风扇青 / 内存玫红 / 上行紫 / 下行蓝 / 温度绿·琥珀·红）——
    // 浅色任务栏用 700 级深底（与白字对比 ≥4.5:1）；深色任务栏用 600 级亮一档，
    // 另加一圈提亮描边，让徽章从近黑任务栏上清晰分离。
    public static Color PowerBadge(bool light) => light ? FromHex(0xC2410C) : FromHex(0xEA580C);

    public static Color FanBadge(bool light) => light ? FromHex(0x0F766E) : FromHex(0x0D9488);

    public static Color MemBadge(bool light) => light ? FromHex(0x9D174D) : FromHex(0xDB2777);

    public static Color NetUpBadge(bool light) => light ? FromHex(0x6D28D9) : FromHex(0x7C3AED);

    public static Color NetDownBadge(bool light) => light ? FromHex(0x1D4ED8) : FromHex(0x2563EB);

    /// <summary>温度三态徽章色：正常绿 / 警告琥珀 / 临界红。</summary>
    public static Color TempBadge(float t, float warn, float crit, bool light)
    {
        if (t >= crit)
            return light ? FromHex(0xB91C1C) : FromHex(0xDC2626);
        if (t >= warn)
            return light ? FromHex(0xB45309) : FromHex(0xD97706);
        return light ? FromHex(0x15803D) : FromHex(0x16A34A);
    }

    private static Color FromHex(int rgb) =>
        Color.FromArgb((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);

    /// <summary>单指标托盘图标：彩色圆盘仪表（渐变盘面 + 深色内环）+ 纯白粗体数字，管家样式，4x 超采样绘制。</summary>
    public static Icon CreateMetricIcon(string text, Color badge, int targetSize, bool lightTheme)
    {
        int size = Math.Clamp(targetSize, 16, 64);
        using Bitmap bmp = Render(size, (g, s) =>
        {
            float m = Margin(s);
            float cx = s / 2f, cy = s / 2f;
            float r = s / 2f - m;

            // 圆盘：垂直渐变（顶部提亮、底部压暗），还原管家监控球的立体感
            using (GraphicsPath disc = Circle(cx, cy, r))
            using (var grad = new LinearGradientBrush(
                new RectangleF(cx - r, cy - r, r * 2f, r * 2f),
                Shade(badge, 0.18f), Shade(badge, -0.10f), LinearGradientMode.Vertical))
            {
                g.FillPath(grad, disc);
            }

            // 同色系深色内环：贴球体边缘的环形色带
            float ringR = r * 0.84f;
            using (Pen ring = new(Shade(badge, -0.30f), MathF.Max(Supersample, r * 0.17f)))
                g.DrawEllipse(ring, cx - ringR, cy - ringR, ringR * 2f, ringR * 2f);

            // 白色粗体数字填满环内空间
            float box = r * 1.18f;
            DrawWhiteText(g, text, new RectangleF(cx - box / 2f, cy - box / 2f, box, box), s * 0.24f, r * 1.30f);
        });
        return ToIcon(bmp);
    }

    /// <summary>网络速率合显图标：上/下两枚胶囊徽章（上行紫、下行蓝）+ 纯白粗体数字。</summary>
    public static Icon CreateNetIcon(string upText, string downText, Color upBadge, Color downBadge,
        int targetSize, bool lightTheme)
    {
        int size = Math.Clamp(targetSize, 16, 64);
        using Bitmap bmp = Render(size, (g, s) =>
        {
            float m = Margin(s);
            float gap = MathF.Max(Supersample, s * 0.055f);
            float h = (s - m * 2 - gap) / 2f;
            RectangleF up = new(m, m, s - m * 2, h);
            RectangleF down = new(m, m + h + gap, s - m * 2, h);
            DrawBadge(g, up, upBadge, lightTheme, h * 0.42f);
            DrawBadge(g, down, downBadge, lightTheme, h * 0.42f);
            float padX = s * 0.06f, padY = s * 0.02f;
            DrawWhiteText(g, upText, new RectangleF(up.X + padX, up.Y + padY, up.Width - padX * 2, up.Height - padY * 2),
                s * 0.09f, h * 0.88f);
            DrawWhiteText(g, downText, new RectangleF(down.X + padX, down.Y + padY, down.Width - padX * 2, down.Height - padY * 2),
                s * 0.09f, h * 0.88f);
        });
        return ToIcon(bmp);
    }

    /// <summary>徽章距图标边缘的留白：至少 1 逻辑像素（超采样坐标下为 Supersample）。</summary>
    private static float Margin(float s) => MathF.Max(Supersample, s * 0.035f);

    /// <summary>在 4x 超采样画布上绘制，再高质量降采样为真实尺寸位图。</summary>
    private static Bitmap Render(int size, Action<Graphics, float> draw)
    {
        int ss = size * Supersample;
        using Bitmap big = new(ss, ss, PixelFormat.Format32bppArgb);
        using (Graphics g = Graphics.FromImage(big))
        {
            g.SmoothingMode = SmoothingMode.HighQuality;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.Clear(Color.Transparent);
            draw(g, ss);
        }

        Bitmap small = new(size, size, PixelFormat.Format32bppArgb);
        using (Graphics g = Graphics.FromImage(small))
        {
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.CompositingQuality = CompositingQuality.HighQuality;
            // TileFlipXY：边缘像素镜像采样，避免透明边缘在降采样时渗色发虚
            using ImageAttributes attr = new();
            attr.SetWrapMode(WrapMode.TileFlipXY);
            Rectangle dest = new(0, 0, size, size);
            g.DrawImage(big, dest, 0, 0, ss, ss, GraphicsUnit.Pixel, attr);
        }

        return small;
    }

    /// <summary>绘制圆角徽章：主题相关的分离描边 + 实色填充。</summary>
    private static void DrawBadge(Graphics g, RectangleF rect, Color badge, bool lightTheme, float radius)
    {
        using GraphicsPath path = RoundRect(rect, radius);
        // 描边：浅色任务栏压暗勾边（徽章与浅底分离），深色任务栏提亮勾边（与近黑底分离）
        float rimWidth = MathF.Max(Supersample, rect.Width * 0.05f);
        using Pen rim = new(Shade(badge, lightTheme ? -0.25f : 0.45f), rimWidth)
        {
            LineJoin = LineJoin.Round,
            StartCap = LineCap.Round,
            EndCap = LineCap.Round,
        };
        g.DrawPath(rim, path);
        using SolidBrush fill = new(badge);
        g.FillPath(fill, path);
    }

    /// <summary>在给定框内绘制纯白粗体数字（自适应字号，尽量占满框）。</summary>
    private static void DrawWhiteText(Graphics g, string text, RectangleF box, float minFont, float maxFont)
    {
        using Font font = FitFont(g, text, box.Width, box.Height, minFont, maxFont);
        using StringFormat sf = new()
        {
            Alignment = StringAlignment.Center,
            LineAlignment = StringAlignment.Center,
            FormatFlags = StringFormatFlags.NoWrap,
            Trimming = StringTrimming.None,
        };
        using GraphicsPath path = new();
        path.AddString(text, new FontFamily(FontFamily), (int)FontStyle.Bold, font.Size, box, sf);
        using SolidBrush brush = new(Color.White);
        g.FillPath(brush, path);
    }

    /// <summary>按给定宽高约束计算可用的最大字号（先探测量再缩放，比按字符数估算更准）。</summary>
    private static Font FitFont(Graphics g, string text, float maxWidth, float maxHeight, float minSize, float maxSize)
    {
        using Font probe = new(FontFamily, 100f, FontStyle.Bold, GraphicsUnit.Pixel);
        SizeF measured = g.MeasureString(text, probe);
        float scale = Math.Min(maxWidth / Math.Max(1f, measured.Width), maxHeight / Math.Max(1f, measured.Height));
        float s = Math.Clamp(100f * scale, minSize, maxSize);
        return new Font(FontFamily, s, FontStyle.Bold, GraphicsUnit.Pixel);
    }

    /// <summary>把颜色向白(+amount)/黑(-amount)方向偏移。</summary>
    private static Color Shade(Color c, float amount)
    {
        float t = Math.Abs(amount);
        int target = amount >= 0 ? 255 : 0;
        return Color.FromArgb(c.A,
            (byte)(c.R + (target - c.R) * t),
            (byte)(c.G + (target - c.G) * t),
            (byte)(c.B + (target - c.B) * t));
    }

    private static GraphicsPath RoundRect(RectangleF r, float radius)
    {
        GraphicsPath path = new();
        float d = radius * 2;
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
        path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
        path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
        path.CloseFigure();
        return path;
    }

    private static GraphicsPath Circle(float cx, float cy, float r)
    {
        GraphicsPath path = new();
        path.AddEllipse(cx - r, cy - r, r * 2f, r * 2f);
        return path;
    }

    /// <summary>把位图封装为 PNG 压缩、尺寸精确的单帧 ICO（透明度完整保留）。</summary>
    private static Icon ToIcon(Bitmap bitmap)
    {
        using MemoryStream stream = new();
        using MemoryStream png = new();
        bitmap.Save(png, ImageFormat.Png);
        byte[] image = png.ToArray();
        using BinaryWriter writer = new(stream, System.Text.Encoding.UTF8, leaveOpen: true);
        writer.Write((ushort)0); // ICONDIR.Reserved
        writer.Write((ushort)1); // ICONDIR.Type
        writer.Write((ushort)1); // ICONDIR.Count
        writer.Write((byte)(bitmap.Width >= 256 ? 0 : bitmap.Width));
        writer.Write((byte)(bitmap.Height >= 256 ? 0 : bitmap.Height));
        writer.Write((byte)0);
        writer.Write((byte)0);
        writer.Write((ushort)1);
        writer.Write((ushort)32);
        writer.Write(image.Length);
        writer.Write(22);
        writer.Write(image);
        writer.Flush();
        stream.Position = 0;
        using Icon icon = new(stream);
        return (Icon)icon.Clone();
    }
}
