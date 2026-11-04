using System.Windows.Controls;

namespace NGenProbe
{
    public partial class MainPage : UserControl
    {
        public MainPage()
        {
            InitializeComponent();
            Probe probe = new Probe();
            int magic = probe.XNGenMagic;
            ValueText.Text = "Value = " + probe.Value;
            TextText.Text = "Text = " + probe.Text;
            MagicText.Text = "XNGenMagic = " + magic;
            VerdictText.Text = magic == 0x5EED ? "Native image code ran" : "Interpreted (no native code)";
            XboxSdk.Samples.DebugLog.WriteLine("NGenProbe: magic " + magic);
        }
    }
}
