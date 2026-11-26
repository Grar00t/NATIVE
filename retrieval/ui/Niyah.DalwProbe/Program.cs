using Niyah.App;

if (args.Length < 2)
{
    Console.WriteLine("usage: probe <shard.dalw> <query> [exact]");
    return 2;
}

Console.OutputEncoding = System.Text.Encoding.UTF8;
using var searcher = new NiyahDalwSearcher(new[] { args[0] });
var json = searcher.SearchJson(args[1], args.Length > 2 && args[2] == "exact", 3);
Console.WriteLine(json);
return 0;
