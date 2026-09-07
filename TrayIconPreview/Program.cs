// 托盘图标 v6 预览生成器：用与主程序完全相同的 TrayIconRenderer 渲染各尺寸图标，
// 铺在浅色/深色任务栏底色上，并输出放大检查行，便于人工核对清晰度与对比度。
using System.Drawing;
using System.Drawing.Drawing2D;
using MonitoringApp;

string outPath = @"C:\Users\<用户名>\Desktop\机械革命监控\托盘图标_v7_预览.png";

var cases = new List<(string Name, string Text, Func<bool, Color> Badge, bool Net)>
{
    ("功耗",     "53",   l => TrayIconRenderer.PowerBadge(l), false),
    ("温度·正常", "54",   l => TrayIconRenderer.TempBadge(54, 75, 90, l), false),
    ("温度·警告", "82",   l => TrayIconRenderer.TempBadge(82, 75, 90, l), false),
    ("温度·临界", "95",   l => TrayIconRenderer.TempBadge(95, 75, 90, l), false),
    ("风扇",     "2.3K", l => TrayIconRenderer.FanBadge(l), false),
    ("内存",     "53",   l => TrayIconRenderer.MemBadge(l), false),
    ("网络·兜底", "1K/4.2K", l => TrayIconRenderer.NetUpBadge(l), true),
};

int[] sizes = [16, 24, 32];
int[] zoomSizes = [16, 24];
const int zoom = 6;
const int labelW = 118, pad = 16, gap = 12, captionH = 30;

Color pageBg = Color.White;
Color lightBar = ColorTranslator.FromHtml("#F3F3F3");
Color darkBar = ColorTranslator.FromHtml("#1F1F1F");
Color captionFg = ColorTranslator.FromHtml("#333A44");
Color rowFg = ColorTranslator.FromHtml("#6B7686");

using Font captionFont = new("Microsoft YaHei UI", 10f, FontStyle.Bold);
using Font rowFont = new("Microsoft YaHei UI", 9f);

// 行布局：type=icon 原始尺寸行；type=zoom 放大行
var rows = new List<(string Caption, string RowLabel, int Size, bool Light, bool Zoom)>();
foreach (int s in sizes)
{
    rows.Add(($"{s}px（{s / 16 * 100}% 缩放）", "浅色", s, true, false));
    rows.Add(("", "深色", s, false, false));
}
string zoomCaption = $"放大 ×{zoom} 检查（16px / 24px）";
foreach (int s in zoomSizes)
{
    rows.Add((s == zoomSizes[0] ? zoomCaption : "", "浅色", s, true, true));
    rows.Add(("", "深色", s, false, true));
}

int W = pad * 2 + labelW;
foreach (var r in rows)
    W = Math.Max(W, pad * 2 + labelW + cases.Count * (r.Size * (r.Zoom ? zoom : 1) + gap));

int H = pad;
foreach (var r in rows)
    H += (r.Caption.Length > 0 ? captionH : 0) + r.Size * (r.Zoom ? zoom : 1) + 14;
H += pad;

using Bitmap bmp = new(W, H, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
using (Graphics g = Graphics.FromImage(bmp))
{
    g.SmoothingMode = SmoothingMode.AntiAlias;
    g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
    g.Clear(pageBg);

    int y = pad;
    foreach (var r in rows)
    {
        int cell = r.Size * (r.Zoom ? zoom : 1);

        if (r.Caption.Length > 0)
        {
            using SolidBrush cb = new(captionFg);
            g.DrawString(r.Caption, captionFont, cb, pad, y);
            y += captionH;
        }

        // 行标签
        using SolidBrush lb = new(rowFg);
        var labelRect = new RectangleF(pad, y, labelW - 10, cell);
        g.DrawString(r.RowLabel, rowFont, lb, labelRect,
            new StringFormat { Alignment = StringAlignment.Near, LineAlignment = StringAlignment.Center });

        // 任务栏底色条
        int barX = pad + labelW - 4, barW = W - barX - pad;
        using (SolidBrush bar = new(r.Light ? lightBar : darkBar))
            g.FillPath(bar, Rounded(new Rectangle(barX, y, barW, cell), 6));

        // 图标
        int x = barX + 8;
        foreach (var c in cases)
        {
            Icon icon = c.Net
                ? TrayIconRenderer.CreateNetIcon("1K", "4.2K",
                    TrayIconRenderer.NetUpBadge(r.Light), TrayIconRenderer.NetDownBadge(r.Light), r.Size, r.Light)
                : TrayIconRenderer.CreateMetricIcon(c.Text, c.Badge(r.Light), r.Size, r.Light);

            using Icon kept = icon;
            using Bitmap tile = kept.ToBitmap();

            if (r.Zoom)
            {
                g.InterpolationMode = InterpolationMode.NearestNeighbor;
                g.PixelOffsetMode = PixelOffsetMode.Half;
                g.DrawImage(tile, new Rectangle(x, y, cell, cell));
                g.InterpolationMode = InterpolationMode.Default;
            }
            else
            {
                g.InterpolationMode = InterpolationMode.NearestNeighbor;
                g.PixelOffsetMode = PixelOffsetMode.Half;
                g.DrawImage(tile, new Rectangle(x, y + (cell - r.Size) / 2, r.Size, r.Size));
            }

            x += cell + gap;
        }

        y += cell + 14;
    }
}

bmp.Save(outPath, System.Drawing.Imaging.ImageFormat.Png);
Console.WriteLine("已生成: " + outPath);
return;

static GraphicsPath Rounded(Rectangle r, int radius)
{
    GraphicsPath p = new();
    float d = radius * 2f;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
    return p;
}
