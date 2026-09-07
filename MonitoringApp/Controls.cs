using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace MonitoringApp;

/// <summary>圆角发光卡片（单个监控指标）。</summary>
public sealed class MetricCard : Control
{
    private string _title = "";
    private string _value = "--";
    private string _unit = "";
    private string _sub = "";
    private Color _accent = Color.White;

    public MetricCard()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        using GraphicsPath path = RoundRect(new Rectangle(0, 0, Width, Height), 12);
        Region = new Region(path);
    }

    public void SetData(string title, string value, string unit, string sub, Color accent)
    {
        _title = title;
        _value = value;
        _unit = unit;
        _sub = sub;
        _accent = accent;
        Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;

        Rectangle r = new(0, 0, Width - 1, Height - 1);
        using GraphicsPath path = RoundRect(r, 12);
        using SolidBrush bg = new(Color.FromArgb(20, 24, 32));
        g.FillPath(bg, path);
        using (Pen border = new(Color.FromArgb(40, 48, 60)))
            g.DrawPath(border, path);

        // 左侧发光竖条
        Rectangle bar = new(12, 18, 5, Height - 36);
        using GraphicsPath barPath = RoundRect(bar, 2);
        using SolidBrush glow = new(Color.FromArgb(50, _accent));
        using (Pen glowPen = new(glow, 7f))
            g.DrawPath(glowPen, barPath);
        using SolidBrush barBrush = new(_accent);
        g.FillPath(barBrush, barPath);

        // 标题
        using Font titleFont = new("Microsoft YaHei UI", 9.5F);
        TextRenderer.DrawText(g, _title, titleFont, new Point(28, 14), Color.FromArgb(158, 165, 178));

        // 数值
        using Font valueFont = new("Microsoft YaHei UI", 25F, FontStyle.Bold);
        Size valueSz = TextRenderer.MeasureText(_value, valueFont);
        TextRenderer.DrawText(g, _value, valueFont, new Point(28, 34), _accent);

        // 单位
        using Font unitFont = new("Microsoft YaHei UI", 10F);
        TextRenderer.DrawText(g, _unit, unitFont, new Point(28 + valueSz.Width + 6, 44), Color.FromArgb(158, 165, 178));

        // 副信息
        using Font subFont = new("Microsoft YaHei UI", 8F);
        TextRenderer.DrawText(g, _sub, subFont, new Point(28, Height - 26), Color.FromArgb(116, 124, 138));
    }

    internal static GraphicsPath RoundRect(Rectangle r, int radius)
    {
        GraphicsPath p = new();
        int d = radius * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }
}

/// <summary>功耗热力图日历（本月，含图例）。</summary>
public sealed class PowerCalendar : Control
{
    private PowerHistory _history = new();
    private DateTime _month = DateTime.Now;

    public PowerCalendar()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        using GraphicsPath path = MetricCard.RoundRect(new Rectangle(0, 0, Width, Height), 12);
        Region = new Region(path);
    }

    public void SetData(PowerHistory history)
    {
        _history = history;
        _month = DateTime.Now;
        Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;

        Rectangle r = new(0, 0, Width - 1, Height - 1);
        using GraphicsPath path = MetricCard.RoundRect(r, 12);
        using SolidBrush bg = new(Color.FromArgb(20, 24, 32));
        g.FillPath(bg, path);
        using (Pen border = new(Color.FromArgb(40, 48, 60)))
            g.DrawPath(border, path);

        using Font title = new("Microsoft YaHei UI", 9.5F);
        TextRenderer.DrawText(g, $"功耗日历 · {_month:yyyy 年 M 月}", title, new Point(16, 12), Color.FromArgb(158, 165, 178));

        int firstDay = (int)new DateTime(_month.Year, _month.Month, 1).DayOfWeek;
        int days = DateTime.DaysInMonth(_month.Year, _month.Month);
        DateTime today = DateTime.Now;

        int cellW = 36, cellH = 28, gap = 6;
        int startX = 16, startY = 40;

        using Font dayFont = new("Microsoft YaHei UI", 8.5F);
        string[] week = ["日", "一", "二", "三", "四", "五", "六"];
        for (int i = 0; i < 7; i++)
        {
            Rectangle wr = new(startX + i * (cellW + gap), startY - 18, cellW, 16);
            TextRenderer.DrawText(g, week[i], dayFont, wr, Color.FromArgb(110, 118, 132), TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        }

        int col = firstDay, row = 0;
        for (int day = 1; day <= days; day++)
        {
            DailyPower? dp = _history.GetDay(new DateTime(_month.Year, _month.Month, day));
            Color cell = ColorFor(dp?.Average);

            Rectangle cellR = new(startX + col * (cellW + gap), startY + row * (cellH + gap), cellW, cellH);
            using GraphicsPath cp = MetricCard.RoundRect(cellR, 6);
            using SolidBrush cb = new(cell);
            g.FillPath(cb, cp);

            bool isToday = today.Year == _month.Year && today.Month == _month.Month && today.Day == day;
            if (isToday)
            {
                using Pen tp = new(Color.FromArgb(0, 255, 157), 2f);
                g.DrawPath(tp, cp);
            }

            Color textColor = dp != null && dp.Average > 0 ? Color.White : Color.FromArgb(116, 124, 138);
            TextRenderer.DrawText(g, day.ToString(), dayFont, cellR, textColor, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);

            col++;
            if (col > 6) { col = 0; row++; }
        }

        // 图例（低 → 高）
        int legendY = startY + 5 * (cellH + gap) + 6;
        using Font legendFont = new("Microsoft YaHei UI", 8F);
        TextRenderer.DrawText(g, "低", legendFont, new Point(startX, legendY), Color.FromArgb(110, 118, 132));
        for (int i = 0; i < 8; i++)
        {
            Rectangle lr = new(startX + 24 + i * 16, legendY + 2, 14, 8);
            using SolidBrush lb = new(ColorFor(60f * i / 7f));
            g.FillRectangle(lb, lr);
        }
        TextRenderer.DrawText(g, "高", legendFont, new Point(startX + 24 + 8 * 16 + 6, legendY), Color.FromArgb(110, 118, 132));
    }

    private static Color ColorFor(float? avg)
    {
        if (avg is null || avg <= 0)
            return Color.FromArgb(32, 38, 48);

        float t = Math.Clamp(avg.Value / 60f, 0, 1);
        return Color.FromArgb(
            40 + (int)(90 * t),
            120 - (int)(70 * t),
            110 - (int)(80 * t));
    }
}
