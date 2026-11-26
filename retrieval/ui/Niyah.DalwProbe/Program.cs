using Niyah.App;

if (args.Length < 2 || args.Skip(2).Any(option => option is not ("exact" or "any")))
{
    Console.WriteLine("usage: probe <shard.dalw> <query> [exact] [any]");
    return 2;
}

Console.OutputEncoding = System.Text.Encoding.UTF8;
using var searcher = new NiyahDalwSearcher(new[] { args[0] });
var json = searcher.SearchJson(args[1], args.Skip(2).Contains("exact"), 3, args.Skip(2).Contains("any"));
Console.WriteLine(json);
return 0;
