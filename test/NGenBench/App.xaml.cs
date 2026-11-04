using System;
using System.Windows;

namespace NGenBench
{
    public partial class App : Application
    {
        public App()
        {
            InitializeComponent();
        }

        private void Application_Startup(object sender, StartupEventArgs e)
        {
            RootVisual = new MainPage();
        }

        private void Application_UnhandledException(object sender, ApplicationUnhandledExceptionEventArgs e)
        {
            e.Handled = true;
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: " + e.ExceptionObject);
        }
    }
}
