using System.Globalization;
using System.Text.Json;
using System.Windows.Markup;
using System.Windows;
using Microsoft.Win32;

namespace Niyah.App;

public partial class MainWindow : Window
{
    private NiyahDalwSearcher? _index;
    private int _shards;
    private bool _busy;
    private bool _closed;

    public MainWindow()
    {
        InitializeComponent();
        var culture = CultureInfo.CurrentUICulture;
        Language = XmlLanguage.GetLanguage(culture.IetfLanguageTag);
        FlowDirection = culture.TextInfo.IsRightToLeft ? FlowDirection.RightToLeft : FlowDirection.LeftToRight;
        Status.Text = Message("Prompt");
        SearchButton.IsEnabled = false;
        Query.TextChanged += (_, _) => CriteriaChanged();
        Exact.Checked += (_, _) => CriteriaChanged();
        Exact.Unchecked += (_, _) => CriteriaChanged();
        MatchAny.Checked += (_, _) => CriteriaChanged();
        MatchAny.Unchecked += (_, _) => CriteriaChanged();
        Loaded += async (_, _) =>
        {
            var paths = Environment.GetEnvironmentVariable("NIYAH_DALW_SHARDS");
            if (!string.IsNullOrWhiteSpace(paths))
                await OpenAsync(paths.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries));
        };
    }

    private string Message(string key, params object[] args) =>
        string.Format(CultureInfo.CurrentUICulture, (string)FindResource(key), args);

    private void CriteriaChanged()
    {
        if (_busy || _index is null) return;
        Results.ItemsSource = null;
        Status.Text = Message("Opened", _shards);
        SearchButton.IsEnabled = !string.IsNullOrWhiteSpace(Query.Text);
    }

    private async void Open_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = Message("Filter"), Multiselect = true };
        if (!_busy && dialog.ShowDialog(this) == true)
            await OpenAsync(dialog.FileNames);
    }

    private async Task OpenAsync(string[] paths)
    {
        if (_busy || paths.Length == 0) return;
        SetBusy(true);
        Status.Text = Message("Opening", paths.Length);
        try
        {
            var next = await Task.Run(() => new NiyahDalwSearcher(paths));
            if (_closed) { next.Dispose(); return; }
            _index?.Dispose();
            _index = next;
            _shards = paths.Length;
            Results.ItemsSource = null;
            Status.Text = Message("Opened", _shards);
        }
        catch (Exception ex) { Status.Text = Message("OpenError", ex.Message); }
        finally
        {
            SetBusy(false);
            if (!_closed && _index != null) Query.Focus();
        }
    }

    private sealed record Hit(int Rank, string Title, string Source, double Score);

    private async void Search_Click(object sender, RoutedEventArgs e)
    {
        var query = Query.Text.Trim();
        var index = _index;
        if (_busy || index is null || query.Length == 0) return;
        bool exact = Exact.IsChecked == true;
        bool any = MatchAny.IsChecked == true;
        SetBusy(true);
        Status.Text = Message("Searching");
        Results.ItemsSource = null;
        try
        {
            string json = await Task.Run(() => index.SearchJson(query, exact, 50, any));
            using var doc = JsonDocument.Parse(json);
            var rows = new List<Hit>();
            foreach (var hit in doc.RootElement.GetProperty("hits").EnumerateArray())
            {
                string title = "", source = "";
                if (hit.TryGetProperty("receipt", out var receipt) && receipt.ValueKind == JsonValueKind.Object)
                {
                    title = ReadString(receipt, "title");
                    source = ReadString(receipt, "url");
                    if (title.Length == 0) title = ReadString(receipt, "id");
                }
                rows.Add(new(hit.GetProperty("rank").GetInt32(), title, source, hit.GetProperty("score").GetDouble()));
            }
            Results.ItemsSource = rows;
            Status.Text = Message("Results", rows.Count, _shards);
        }
        catch (Exception ex) { Status.Text = Message("SearchError", ex.Message); }
        finally { SetBusy(false); }
    }

    private static string ReadString(JsonElement obj, string key) =>
        obj.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";

    private void SetBusy(bool busy)
    {
        _busy = busy;
        OpenButton.IsEnabled = !busy;
        Query.IsEnabled = !busy;
        Exact.IsEnabled = !busy;
        MatchAny.IsEnabled = !busy;
        SearchButton.IsEnabled = !busy && _index != null && !string.IsNullOrWhiteSpace(Query.Text);
        if (!busy && _closed) { _index?.Dispose(); _index = null; }
    }

    protected override void OnClosed(EventArgs e)
    {
        _closed = true;
        if (!_busy) { _index?.Dispose(); _index = null; }
        base.OnClosed(e);
    }
}
