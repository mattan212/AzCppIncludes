using System.Windows;
using System.Windows.Input;

namespace AzCppIncludesVsix.Xaml
{
    public partial class NotificationDialog : Window
    {
        private readonly string _header;
        private readonly string _content;

        public NotificationDialog(string header, string content)
        {
            _header = content;
            _content = content;
            
            InitializeComponent();

            Loaded += (s, e) =>
            {
                InfoText.Text = _content;
                Title = header;
            };
        }

        private void OnKeyDownHandler(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Escape)
            {
                DialogResult = false;
                Close();
            }
        }

        private void Button_Click(object sender, RoutedEventArgs e)
        {
            DialogResult = true;
            Close();
        }
    }
}

