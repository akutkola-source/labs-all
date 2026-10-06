#include "MainForm.h"

using namespace System;
using namespace System::Windows::Forms;

[STAThreadAttribute]
int main(array<String^>^ args) {
    Application::EnableVisualStyles();
    Application::SetCompatibleTextRenderingDefault(false);

    // age - это ваше пространство имен (видно на скриншоте)
    age::MainForm form;
    Application::Run(% form);
    return 0;
