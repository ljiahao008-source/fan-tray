using LibreHardwareMonitor.Mechrevo;

using MonitoringService service = new();
Console.WriteLine("机械革命精简监控库测试（读 5 次）");

for (int i = 0; i < 5; i++)
{
    MonitoringSnapshot s = service.Read();
    Console.WriteLine(s.ToChineseString());
    Thread.Sleep(1000);
}
