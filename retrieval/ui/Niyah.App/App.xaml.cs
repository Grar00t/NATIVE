using System.Globalization;
using System.Windows;

namespace Niyah.App;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        string locale = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "ar" ? "ar" : "en";
        if (e.Args is ["--language", "ar" or "en"]) locale = e.Args[1];
        CultureInfo.CurrentUICulture = CultureInfo.GetCultureInfo(locale);
        Resources.MergedDictionaries.Add(new ResourceDictionary {
            Source = new Uri($"/Niyah.App;component/Text.{locale}.xaml", UriKind.Relative)
        });
        base.OnStartup(e);
    }
}
