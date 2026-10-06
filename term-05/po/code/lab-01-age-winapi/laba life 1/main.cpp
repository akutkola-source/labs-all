
#include <windows.h>
#include "Controller.h"

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int nCmdShow)
{
    Controller controller(hInstance);

    if (!controller.Initialize())
        return 1;

    return controller.Run(nCmdShow);
}