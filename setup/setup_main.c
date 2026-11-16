/* FearEffect-Setup.exe: start the setup window with no console.
   RecompLauncher looks for *Setup.exe in the install folder. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    wchar_t dir[MAX_PATH];
    wchar_t *slash;
    INT_PTR rc;

    (void)inst;
    (void)prev;
    (void)cmd;
    (void)show;

    if (!GetModuleFileNameW(NULL, dir, MAX_PATH))
        return 1;
    slash = wcsrchr(dir, L'\\');
    if (slash)
        *slash = 0;
    SetCurrentDirectoryW(dir);

    rc = (INT_PTR)ShellExecuteW(NULL, L"open", L"setup\\silent.vbs", NULL, dir, SW_HIDE);
    if (rc <= 32)
        rc = (INT_PTR)ShellExecuteW(NULL, L"open", L"setup.bat", NULL, dir, SW_SHOWNORMAL);
    if (rc <= 32) {
        MessageBoxW(
            NULL,
            L"Could not start setup\\silent.vbs.\n"
            L"Keep FearEffect-Setup.exe next to the setup folder.",
            L"Fear Effect Setup",
            MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}
