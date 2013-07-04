/*
 * Copyright (c) 2010 Secure Endpoints Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define UNICODE
#define _UNICODE

#include<windows.h>
#include<psapi.h>
#include<iostream>
#include<fstream>
#include<string>
#include<time.h>
#include<wchar.h>

#pragma comment(lib, "advapi32")
#pragma comment(lib, "psapi")

static std::wstring g_java_home;
static std::wstring g_gerrit_site;
static std::wstring g_gerrit_war;
static std::wstring g_service_name;
static std::wstring g_display_name;
static std::wstring g_account_name;
static std::wstring g_password;
static DWORD        g_start_type = SERVICE_AUTO_START;

static SERVICE_STATUS_HANDLE g_status = NULL;

static HANDLE g_shutdown_event = NULL;
static HANDLE g_wait_object = NULL;

static HANDLE g_job = NULL;
static HANDLE g_process = NULL;
static HANDLE g_completion_port = NULL;

static std::wofstream logfile;

wchar_t *
timestamp()
{
    time_t t;
    static wchar_t buf[128];
    size_t l;

    time(&t);
    _wctime_s(buf, 128, &t);

    l = wcslen(buf);
    if (l > 0 && buf[l-1] == L'\n')
        buf[l-1] = L'\0';

    return buf;
}

void
open_logfile(bool log_started)
{
    std::wstring lpath = g_gerrit_site;
    lpath += L"\\logs\\gerrit_service_log";

    logfile.open(lpath.c_str(), std::ios_base::app);

    if (log_started)
        logfile << timestamp() << L" Logging started" << std::endl;
}

void
set_service_state(DWORD state, DWORD checkpoint = 0,
                  DWORD wait_hint = 0, DWORD exit_code = 0)
{
    SERVICE_STATUS status;

    ZeroMemory(&status, sizeof(status));

    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = NO_ERROR;
    status.dwCheckPoint = checkpoint;
    status.dwWaitHint = wait_hint;

    switch (state) {
    case SERVICE_CONTINUE_PENDING:
        status.dwControlsAccepted = 0;
        break;

    case SERVICE_PAUSE_PENDING:
        status.dwControlsAccepted = 0;
        break;

    case SERVICE_PAUSED:
        status.dwControlsAccepted = SERVICE_ACCEPT_PAUSE_CONTINUE;
        break;

    case SERVICE_RUNNING:
        status.dwControlsAccepted = SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_STOP;
        break;

    case SERVICE_START_PENDING:
        status.dwControlsAccepted = 0;
        break;

    case SERVICE_STOP_PENDING:
        status.dwControlsAccepted = 0;
        break;

    case SERVICE_STOPPED:
        status.dwControlsAccepted = 0;
        if (exit_code != 0) {
            status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
            status.dwServiceSpecificExitCode = exit_code;
        }
        break;

    default:
        return;
    }

    SetServiceStatus(g_status, &status);
}

void
stop_svc()
{
    if (g_process && g_job) {
        set_service_state(SERVICE_STOP_PENDING, 1, 3000);

        SetConsoleCtrlHandler(NULL, TRUE);
        if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
            logfile << timestamp()
                    << " Can't send Ctrl-C to child process.  Terminating child. Gle="
                    << GetLastError() << std::endl;
            TerminateJobObject(g_job, 0);
        } else {
            logfile << timestamp()
                    << " Sent Ctrl-C to child process." << std::endl;
        }
        return;
    }

    set_service_state(SERVICE_STOP_PENDING, 2, 1000);

    if (g_job) {
        CloseHandle(g_job);
        g_job = NULL;
    }

    if (g_completion_port) {
        CloseHandle(g_completion_port);
        g_completion_port = NULL;
    }

    if (g_shutdown_event) {
        CloseHandle(g_shutdown_event);
        g_shutdown_event = NULL;
    }

    set_service_state(SERVICE_STOPPED);
}

DWORD WINAPI
svc_ctrl_handler(__in  DWORD dwControl, __in  DWORD dwEventType,
                 __in  LPVOID lpEventData, __in  LPVOID lpContext)
{
    switch (dwControl) {
    case SERVICE_CONTROL_CONTINUE:
        break;

    case SERVICE_CONTROL_INTERROGATE:
        break;

    case SERVICE_CONTROL_PAUSE:
        break;

    case SERVICE_CONTROL_STOP:
        set_service_state(SERVICE_STOP_PENDING, 0, 3000);
        SetEvent(g_shutdown_event);
        break;
    }

    return NO_ERROR;
}

// Gets called when the g_shutdown_event is set
VOID CALLBACK
svc_termination_handler(__in  PVOID lpParameter, __in  BOOLEAN TimerOrWaitFired)
{
    stop_svc();
}

void
setup_environment()
{
    std::wstring regpath = L"System\\CurrentControlSet\\Services\\";
    HKEY hk_env = 0;

    regpath += g_service_name;

    regpath += L"\\Parameters\\Environment";

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, regpath.c_str(),
                     0, KEY_READ, &hk_env) != ERROR_SUCCESS) {
        logfile << L"Can't open environment registry key" << std::endl;
        return;
    }

    wchar_t name[128];
    wchar_t value[2048];
    DWORD cch_name = sizeof(name)/sizeof(name[0]);
    DWORD cch_value = sizeof(value)/sizeof(value[0]);
    DWORD type = 0;

    for (DWORD idx = 0;
         RegEnumValue(hk_env, idx, name, &cch_name, NULL, &type,
                      (LPBYTE) value, &cch_value) == ERROR_SUCCESS;
         idx++,
             cch_name = sizeof(name)/sizeof(name[0]),
             cch_value = sizeof(value)/sizeof(value[0])
         ) {

        if (cch_name <= 1)
            continue;

        if (type == REG_EXPAND_SZ) {
            wchar_t evalue[2048];

            if (ExpandEnvironmentStrings(value, evalue,
                                         sizeof(evalue)/sizeof(evalue[0])) != 0) {
                wcscpy_s(value, evalue);
            }
        } else if (type != REG_SZ) {
            continue;
        }

        logfile << L"Setting environment variable: " << name << " = " << value
                << std::endl;
        SetEnvironmentVariable(name, value);
    }

    RegCloseKey(hk_env);
}

void
run_gerrit()
{
    bool process_running = false;

    setup_environment();

    if (!AllocConsole()) {
        logfile << "Can't allocate console. Gle=" << GetLastError() << std::endl;
        return;
    }

    g_job = CreateJobObject(NULL, L"Gerrit job object");

    if (g_job == NULL) {
        logfile << "Can't create job object. Gle=" << GetLastError() << std::endl;
        return;
    }

    g_completion_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (g_completion_port == NULL) {
        logfile << "Can't create IO completion port. Gle=" << GetLastError() << std::endl;
        return;
    }
    
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT acp;

    acp.CompletionKey = NULL;
    acp.CompletionPort = g_completion_port;

    if (!SetInformationJobObject(g_job, JobObjectAssociateCompletionPortInformation,
                                 &acp, sizeof(acp))) {
        logfile << "Can't associate IO completion port. Gle=" << GetLastError() << std::endl;
        return;
    }

    std::wstring cmdline;
    wchar_t * wcmdline = NULL;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;

    cmdline.append(L"\"");
    if (g_java_home.size() != 0) {
        cmdline.append(g_java_home);
        cmdline.append(L"\\bin\\");
    }
    cmdline.append(L"java.exe\" -jar \"");
    cmdline.append(g_gerrit_war);
    cmdline.append(L"\" daemon -d \"");
    cmdline.append(g_gerrit_site);
    cmdline.append(L"\"");

    wcmdline = new wchar_t[cmdline.size()+1];

    cmdline.copy(wcmdline, cmdline.size());
    wcmdline[cmdline.size()] = L'\0';

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    ZeroMemory(&pi, sizeof(pi));

    do {
        logfile << timestamp()
                << " Attempting to create process [" << wcmdline << "]" << std::endl;

        if (!CreateProcess(NULL, wcmdline, NULL, NULL, FALSE,
                           CREATE_SUSPENDED,
                           NULL,
                           g_gerrit_site.c_str(),
                           &si, &pi)) {
            logfile << timestamp()
                    << " Failed to create process. Gle=" << std::endl;
            break;
        }

        if (!AssignProcessToJobObject(g_job, pi.hProcess)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            ZeroMemory(&pi, sizeof(pi));
            break;
        }

        ResumeThread(pi.hThread);

        CloseHandle(pi.hThread);
        g_process = pi.hProcess;
        process_running = true;

    } while (false);

    delete [] wcmdline;

    while (process_running) {
        DWORD n = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED po = NULL;

        logfile.close();
        if (!GetQueuedCompletionStatus(g_completion_port, &n, &key, &po, INFINITE) &&
            po == NULL) {
            process_running = false;
            open_logfile(false);
            break;
        }
        open_logfile(false);

        if (n == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) {
            logfile << timestamp()
                    << " Received MSG_ACTIVE_PROCESS_ZERO.  Terminating"
                    << std::endl;

            process_running = false;
            if (g_process) {
                CloseHandle(g_process);
                g_process = NULL;
            }
            break;
        }

        logfile << timestamp()
                << " Continuing after GetQueuedCompletionStatus() "
                << ";n = " << n
                << ";key = " << key
                << ";po = " << po << std::endl;
    }

    return;
}

VOID WINAPI
gerrit_svc_main(__in  DWORD dwArgc, __in  LPTSTR* lpszArgv)
{
    open_logfile(true);

    logfile << timestamp() << " Starting service" << std::endl;

    g_status = RegisterServiceCtrlHandlerEx(g_service_name.c_str(),
                                            svc_ctrl_handler, NULL);
    if (!g_status) {
        logfile << "Can't register service control handler. Gle=" << GetLastError()
                << std::endl;
        return;
    }

    g_shutdown_event = CreateEvent(NULL, FALSE, FALSE,
                                   L"Gerrit service shutdown event");

    if (!RegisterWaitForSingleObject(&g_wait_object, g_shutdown_event,
                                     svc_termination_handler,
                                     NULL, INFINITE,
                                     WT_EXECUTEONLYONCE| WT_EXECUTEDEFAULT)) {
        logfile << "RegisterWaitForSingleObject() failed. Gle=" << GetLastError()
                << std::endl;
        return;
    }

    set_service_state(SERVICE_RUNNING, 0, 0);

    logfile << timestamp() << " Service running" << std::endl;

    run_gerrit();

    stop_svc();
}

void
show_usage(const wchar_t * pname)
{
    std::wcout
        << pname << L": Runn Gerrit Code Review" << std::endl
        << L"Usage: " << pname << L" -d GERRIT_SITE [options...]" << std::endl
        << L"   -j <JAVA_HOME>   : Set JAVA_HOME" << std::endl
        << L"   -d <GERRIT_SITE> : Set GERRIT_SITE" << std::endl
        << L"   -g <path>        : Set the path to gerrit.war.  Defaults to $GERRIT_SITE\\bin\\gerrit.war" << std::endl
        << L"   -i               : Installs the service.  Cannot be used with -u." << std::endl
        << L"   -u               : Uninstall the service.  Cannot be used with -i." << std::endl
        // << std::endl
        // << L"The following options are used with -i and -u :" << std::endl
        // << L"   -s <service name>: Set the name of the service.  Defaults to GerritService." << std::endl
        << std::endl
        << L"The following options are used with -i :" << std::endl
        << L"   -S <display name>: Set the display name of the service." << std::endl
        << L"                      Defaults to \"Gerrit Code Review Service\".  The name must be quoted.  Can only be used with -i." << std::endl
        << L"   -a <account>     : Set the account name under which the service will run." << std::endl
        << "                       Defaults to \"NT AUTHORITY\\LocalSystem\"." << std::endl
        << L"   -p <password>    : The password for the account.  Defaults to the empty string." << std::endl
        << std::endl;
}

int
uninstall_service()
{
    SC_HANDLE scm = NULL;
    SC_HANDLE svc = NULL;

    std::wcout << "Preparing to delete service ... " << g_service_name << std::endl;

    do {

        scm = OpenSCManager(NULL, NULL, SC_MANAGER_LOCK | SC_MANAGER_CONNECT);
        if (scm == NULL) {
            std::cerr << "Can't connect to SCM.  GLE=" << GetLastError() << std::endl;
            break;
        }

        svc = OpenService(scm, g_service_name.c_str(), SERVICE_STOP | SERVICE_INTERROGATE | DELETE);
        if (svc == NULL) {
            DWORD gle = GetLastError();

            if (gle == ERROR_SERVICE_DOES_NOT_EXIST)
                std::cerr << "Service is not installed" << std::endl;
            else
                std::cerr << "Can't open service. GLE=" << gle << std::endl;
            break;
        }

        std::cout << "Stopping service .";

        SERVICE_STATUS status;

        if (!ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
            DWORD gle = GetLastError();

            if (gle == ERROR_SERVICE_NOT_ACTIVE) {
                std::cout << "Service is not running" << std::endl;
            } else {
                std::cerr << std::endl << "Can't send stop signal to service. GLE=" << gle
                          << std::endl;
            }
        }

        while (status.dwCurrentState == SERVICE_STOP_PENDING) {
            Sleep(1000);
            std::cout << ".";
            QueryServiceStatus(svc, &status);
        }

        std::cout << "Stopped" << std::endl;

        if (!DeleteService(svc)) {
            std::cerr << "Can't delete service. GLE=" << GetLastError() << std::endl;
        } else {
            std::cout << "Service successfully removed" << std::endl;
        }

    } while (false);

    if (svc != NULL)
        CloseServiceHandle(svc);

    if (scm != NULL)
        CloseServiceHandle(scm);

    return 0;
}

int
install_service()
{
    std::wstring cmdline;

    if (g_display_name.size() == 0)
        g_display_name = L"Gerrit Code Review Service";

    {
        wchar_t path[MAX_PATH];

        if (GetModuleFileNameEx(GetCurrentProcess(), NULL, path, MAX_PATH) == 0) {
            std::cerr << "Can't determine module path" << std::endl;
            return 0;
        }

        cmdline.append(L"\"");
        cmdline.append(path);
        cmdline.append(L"\"");
    }

    if (g_java_home.size() != 0)
        cmdline.append(L" -j \"" + g_java_home + L"\"");
    cmdline.append(L" -d \"" + g_gerrit_site + L"\"");
    if (g_gerrit_war.size() != 0)
        cmdline.append(L" -g \"" + g_gerrit_war + L"\"");

    std::wcout << "Attempting to install Gerrit service: " << std::endl
               << "  Service name: " << g_service_name << std::endl
               << "  Display name: " << g_display_name << std::endl
               << "  Account     : " << g_account_name << std::endl
               << "  Gerrit site : " << g_gerrit_site << std::endl
               << "  gerrit.war  : " << g_gerrit_war << std::endl
               << "  Java path   : " << g_java_home << std::endl
               << "  Command line: " << cmdline << std::endl;

    SC_HANDLE scm = NULL;
    SC_HANDLE svc = NULL;

    do {

        scm = OpenSCManager(NULL, NULL,
                            SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
        if (scm == NULL) {
            std::cerr << "Can't connect to SCM.  GLE="
                      << GetLastError() << std::endl;
            break;
        }

        svc = CreateService(scm,
                            g_service_name.c_str(),
                            g_display_name.c_str(),
                            SERVICE_CHANGE_CONFIG |
                            SERVICE_INTERROGATE |
                            SERVICE_START |
                            SERVICE_QUERY_STATUS,
                            SERVICE_WIN32_OWN_PROCESS,
                            g_start_type,
                            SERVICE_ERROR_NORMAL,
                            cmdline.c_str(),
                            NULL,
                            NULL,
                            NULL,
                            (g_account_name.size() > 0 ? g_account_name.c_str() : NULL),
                            g_password.c_str());
        if (svc == NULL) {
            DWORD gle = GetLastError();

            if (gle == ERROR_SERVICE_EXISTS)
                std::cerr << std::endl
                          << "Service already exists.  Use the -u option to remove before installing" << std::endl;
            else
                std::cerr << "Can't create service.  GLE=" << GetLastError() << std::endl;
            break;
        }

        std::cout << "Service created." << std::endl
                  << "Attempting to start service :" << std::endl
            ;

        if (!StartService(svc, 0, NULL)) {
            std::cerr << std::endl
                      << "Can't start service. GLE=" << GetLastError() << std::endl;
            break;
        }

        while (TRUE) {
            SERVICE_STATUS status;

            if (!QueryServiceStatus(svc, &status)) {
                std::cout << std::endl;
                std::cerr << "Error while querying service status. GLE=" << GetLastError() << std::endl;
                break;
            }

            if (status.dwCurrentState == SERVICE_RUNNING) {
                std::cout << std::endl << "Service started successfully." << std::endl;
                break;
            }

            if (status.dwCurrentState == SERVICE_STOP_PENDING ||
                status.dwCurrentState == SERVICE_STOPPED) {
                std::cout << std::endl;
                std::cerr << "Service stopped unexpectedly" << std::endl;
                break;
            }

            if (status.dwCurrentState != SERVICE_START_PENDING) {
                std::cout << std::endl;
                std::cerr << "Error while starting service. Service statue=" << status.dwCurrentState << std::endl;
                break;
            }

            Sleep(1000);
            std::cout << ".";
        }

    } while(false);

    if (svc != NULL)
        CloseServiceHandle(svc);

    if (scm != NULL)
        CloseServiceHandle(scm);

    return 0;
}

int
process_arguments(int argc, wchar_t ** argv)
{
    bool install = false;
    bool uninstall = false;

    for (int i = 1; i < argc; ++i) {

        if (!wcscmp(argv[i], L"-j") && i + 1 < argc) {

            g_java_home = argv[++i];
            
        } else if (!wcscmp(argv[i], L"-d") && i + 1 < argc) {

            g_gerrit_site = argv[++i];

        } else if (!wcscmp(argv[i], L"-g") && i + 1 < argc) {

            g_gerrit_war = argv[++i];

        }
        /* else if (!wcscmp(argv[i], L"-s") && i + 1 < argc) {

           g_service_name = argv[++i];

           } */
        else if (!wcscmp(argv[i], L"-S") && i + 1 < argc) {

            g_display_name = argv[++i];

        } else if (!wcscmp(argv[i], L"-a") && i + 1 < argc) {

            g_account_name = argv[++i];

        } else if (!wcscmp(argv[i], L"-p") && i + 1 < argc) {

            g_password = argv[++i];

        } else if (!wcscmp(argv[i], L"-m")) {

            g_start_type = SERVICE_DEMAND_START;

        } else if (!wcscmp(argv[i], L"-i")) {

            install = true;
            if (uninstall) {
                show_usage(argv[0]);
                return 0;
            }

        } else if (!wcscmp(argv[i], L"-u")) {

            uninstall = true;
            if (install) {
                show_usage(argv[0]);
                return 0;
            }

        } else {

            show_usage(argv[0]);
            return 0;

        }
    }

    if (g_service_name.size() == 0)
        g_service_name = L"GerritService";

    if (uninstall) {
        uninstall_service();
        return 0;
    }

    if (g_gerrit_site.size() == 0) {
        show_usage(argv[0]);
        return 0;
    }

    if (g_gerrit_war.size() == 0) {
        g_gerrit_war = g_gerrit_site + L"\\bin\\gerrit.war";
    }

    if (!install)
        return 1;

    install_service();
    return 0;
}

int
run_service(void)
{
    SERVICE_TABLE_ENTRY svc[] = {
        { L"Gerrit", gerrit_svc_main },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcher(svc)) {
        std::cerr << "Can't start service. Gle=" << GetLastError() << std::endl;
        return 0;
    }

    return 1;
}

int
wmain(int argc, wchar_t ** argv)
{
    if (!process_arguments(argc, argv))
        return 1;

    return !run_service();
}
