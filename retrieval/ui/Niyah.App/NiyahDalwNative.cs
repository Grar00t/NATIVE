using System.Runtime.InteropServices;
using System.Text;

namespace Niyah.App;

internal sealed class DalwHandle : SafeHandle
{
    public DalwHandle() : base(IntPtr.Zero, true) { }
    public override bool IsInvalid => handle == IntPtr.Zero;
    protected override bool ReleaseHandle()
    {
        NiyahDalwNative.niyah_dalw_close(handle);
        return true;
    }
}

internal static class NiyahDalwNative
{
    private const string Dll = "niyah_dalw";
    internal const int InsufficientMemory = -6;

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern DalwHandle niyah_dalw_open(IntPtr[] paths, nuint n, int verify, out int status);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void niyah_dalw_close(IntPtr handle);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int niyah_dalw_search_json_mode(DalwHandle handle, byte[] query, int exact, int any, nuint k,
        [Out] byte[]? output, nuint cap, out int status);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr niyah_dalw_strerror(int status);
}

public sealed class NiyahDalwException : Exception
{
    public int Status { get; }

    public NiyahDalwException(int status, string message) : base(message)
    {
        Status = status;
    }
}

/// <summary>
/// Opens dalw shards and returns search results as one JSON string per query.
/// Shard paths are passed to the native reader as UTF-8.
/// </summary>
public sealed class NiyahDalwSearcher : IDisposable
{
    private readonly DalwHandle _handle;

    public NiyahDalwSearcher(IReadOnlyList<string> shardPaths, bool verify = true)
    {
        if (shardPaths is null || shardPaths.Count == 0)
            throw new ArgumentException("at least one shard is required", nameof(shardPaths));

        var ptrs = new IntPtr[shardPaths.Count];
        try
        {
            for (int i = 0; i < ptrs.Length; i++)
            {
                if (string.IsNullOrEmpty(shardPaths[i]) || shardPaths[i].Contains('\0'))
                    throw new ArgumentException("invalid shard path", nameof(shardPaths));
                ptrs[i] = Marshal.StringToCoTaskMemUTF8(shardPaths[i]);
            }

            _handle = NiyahDalwNative.niyah_dalw_open(ptrs, (nuint)ptrs.Length, verify ? 1 : 0, out int st);
            if (_handle.IsInvalid) { _handle.Dispose(); throw Fail(st); }
        }
        finally
        {
            foreach (var p in ptrs)
                if (p != IntPtr.Zero) Marshal.FreeCoTaskMem(p);
        }
    }

    /// <param name="exact">true disables Arabic stemming.</param>
    public string SearchJson(string query, bool exact = false, int k = 10) => SearchJson(query, exact, k, false);

    public string SearchJson(string query, bool exact, int k, bool matchAny)
    {
        ObjectDisposedException.ThrowIf(_handle.IsClosed, this);
        ArgumentNullException.ThrowIfNull(query);
        if (query.Contains('\0')) throw new ArgumentException("query contains NUL", nameof(query));
        k = Math.Clamp(k, 1, 1000);

        var q = new byte[Encoding.UTF8.GetByteCount(query) + 1];
        Encoding.UTF8.GetBytes(query, 0, query.Length, q, 0);
        var buf = new byte[16384];

        int n = NiyahDalwNative.niyah_dalw_search_json_mode(_handle, q, exact ? 1 : 0, matchAny ? 1 : 0, (nuint)k,
            buf, (nuint)buf.Length, out int st);
        if (n < 0 && st == NiyahDalwNative.InsufficientMemory)
        {
            n = NiyahDalwNative.niyah_dalw_search_json_mode(_handle, q, exact ? 1 : 0, matchAny ? 1 : 0,
                (nuint)k, null, 0, out st);
            if (n < 0) throw Fail(st);
            buf = new byte[checked(n + 1)];
            n = NiyahDalwNative.niyah_dalw_search_json_mode(_handle, q, exact ? 1 : 0, matchAny ? 1 : 0,
                (nuint)k, buf, (nuint)buf.Length, out st);
        }
        if (n < 0) throw Fail(st);
        return Encoding.UTF8.GetString(buf, 0, n);
    }

    private static NiyahDalwException Fail(int st)
    {
        var msg = Marshal.PtrToStringUTF8(NiyahDalwNative.niyah_dalw_strerror(st)) ?? "unknown error";
        return new NiyahDalwException(st, msg);
    }

    public void Dispose() => _handle.Dispose();
}
