/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

#define INITGUID

#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>
#include <malloc.h>

#include <version.h>

__user_code;

#define MAXIMUM_BUFFER_SIZE 1024

#define PNP_KEY      "SYSTEM\\CurrentControlSet\\Control\\PnP"
#define SERVICES_KEY "SYSTEM\\CurrentControlSet\\Services"

#define SERVICE_KEY(_Driver)    \
        SERVICES_KEY ## "\\" ## #_Driver

#define UNPLUG_KEY(_Driver)     \
        SERVICE_KEY(_Driver) ## "\\Unplug"

static VOID
#pragma prefast(suppress:6262) // Function uses '1036' bytes of stack: exceeds /analyze:stacksize'1024'
__Log(
    IN  const CHAR  *Format,
    IN  ...
    )
{
    TCHAR               Buffer[MAXIMUM_BUFFER_SIZE];
    va_list             Arguments;
    size_t              Length;
    SP_LOG_TOKEN        LogToken;
    DWORD               Category;
    DWORD               Flags;
    HRESULT             Result;

    va_start(Arguments, Format);
    Result = StringCchVPrintf(Buffer, MAXIMUM_BUFFER_SIZE, Format, Arguments);
    va_end(Arguments);

    if (Result != S_OK && Result != STRSAFE_E_INSUFFICIENT_BUFFER)
        return;

    Result = StringCchLength(Buffer, MAXIMUM_BUFFER_SIZE, &Length);
    if (Result != S_OK)
        return;

    LogToken = SetupGetThreadLogToken();
    Category = TXTLOG_VENDOR;
    Flags = TXTLOG_DETAILS;

    SetupWriteTextLog(LogToken, Category, Flags, Buffer);
    Length = __min(MAXIMUM_BUFFER_SIZE - 1, Length + 2);

    __analysis_assume(Length < MAXIMUM_BUFFER_SIZE);
    __analysis_assume(Length >= 2);
    Buffer[Length] = '\0';
    Buffer[Length - 1] = '\n';
    Buffer[Length - 2] = '\r';

    OutputDebugString(Buffer);
}

#define Log(_Format, ...) \
        __Log(__MODULE__ "|" __FUNCTION__ ": " _Format, __VA_ARGS__)

static PTCHAR
GetErrorMessage(
    IN  DWORD   Error
    )
{
    PTCHAR      Message;
    ULONG       Index;

    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | 
                  FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL,
                  Error,
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  (LPTSTR)&Message,
                  0,
                  NULL);

    for (Index = 0; Message[Index] != '\0'; Index++) {
        if (Message[Index] == '\r' || Message[Index] == '\n') {
            Message[Index] = '\0';
            break;
        }
    }

    return Message;
}

static const CHAR *
FunctionName(
    IN  DI_FUNCTION Function
    )
{
#define _NAME(_Function)        \
        case DIF_ ## _Function: \
            return #_Function;

    switch (Function) {
    _NAME(INSTALLDEVICE);
    _NAME(REMOVE);
    _NAME(SELECTDEVICE);
    _NAME(ASSIGNRESOURCES);
    _NAME(PROPERTIES);
    _NAME(FIRSTTIMESETUP);
    _NAME(FOUNDDEVICE);
    _NAME(SELECTCLASSDRIVERS);
    _NAME(VALIDATECLASSDRIVERS);
    _NAME(INSTALLCLASSDRIVERS);
    _NAME(CALCDISKSPACE);
    _NAME(DESTROYPRIVATEDATA);
    _NAME(VALIDATEDRIVER);
    _NAME(MOVEDEVICE);
    _NAME(DETECT);
    _NAME(INSTALLWIZARD);
    _NAME(DESTROYWIZARDDATA);
    _NAME(PROPERTYCHANGE);
    _NAME(ENABLECLASS);
    _NAME(DETECTVERIFY);
    _NAME(INSTALLDEVICEFILES);
    _NAME(ALLOW_INSTALL);
    _NAME(SELECTBESTCOMPATDRV);
    _NAME(REGISTERDEVICE);
    _NAME(NEWDEVICEWIZARD_PRESELECT);
    _NAME(NEWDEVICEWIZARD_SELECT);
    _NAME(NEWDEVICEWIZARD_PREANALYZE);
    _NAME(NEWDEVICEWIZARD_POSTANALYZE);
    _NAME(NEWDEVICEWIZARD_FINISHINSTALL);
    _NAME(INSTALLINTERFACES);
    _NAME(DETECTCANCEL);
    _NAME(REGISTER_COINSTALLERS);
    _NAME(ADDPROPERTYPAGE_ADVANCED);
    _NAME(ADDPROPERTYPAGE_BASIC);
    _NAME(TROUBLESHOOTER);
    _NAME(POWERMESSAGEWAKE);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _NAME
}

static BOOLEAN
RequestUnplug(
    VOID          
    )
{
    HKEY    UnplugKey;
    DWORD   DisksLength;
    PTCHAR  Disks;
    DWORD   Offset;
    HRESULT Error;
    HRESULT Result;

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         UNPLUG_KEY(XENFILT),
                         0,
                         KEY_ALL_ACCESS,
                         &UnplugKey);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    DisksLength = (DWORD)((strlen("XENVBD") + 1 +
                           1) * sizeof (TCHAR));

    Disks = calloc(1, DisksLength);
    if (Disks == NULL)
        goto fail2;

    Offset = 0;

    Result = StringCbPrintf(Disks + Offset,
                            DisksLength - (Offset * sizeof (TCHAR)),
                            "XENVBD");
    if (!SUCCEEDED(Result)) {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        goto fail3;
    }

    Offset += (DWORD)(strlen("XENVBD") + 1);

    *(Disks + Offset) = '\0';

    Error = RegSetValueEx(UnplugKey,
                          "DISKS",
                          0,
                          REG_MULTI_SZ,
                          (LPBYTE)Disks,
                          DisksLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail4;
    }

    free(Disks);

    RegCloseKey(UnplugKey);

    return TRUE;

fail4:
    Log("fail4");

fail3:
    Log("fail3");

    free(Disks);

fail2:
    Log("fail2");

    RegCloseKey(UnplugKey);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
ModifyBootGPO(
    )
{
    HKEY        PnpKey;
    DWORD       Value = 0;
    HRESULT     Error;

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         PNP_KEY,
                         0,
                         KEY_ALL_ACCESS,
                         &PnpKey);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    Error = RegSetValueEx(PnpKey,
                          "DisableCDDB",
                          0,
                          REG_DWORD,
                          (LPBYTE)&Value,
                          (DWORD)sizeof(DWORD));
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail2;
    }

    Error = RegSetValueEx(PnpKey,
                          "DontStartRawDevices",
                          0,
                          REG_DWORD,
                          (LPBYTE)&Value,
                          (DWORD)sizeof(DWORD));
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail3;
    }

    RegCloseKey(PnpKey);

    return TRUE;

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    RegCloseKey(PnpKey);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
IncrementServiceCount(
    OUT PDWORD  Count
    )
{
    HKEY        ServiceKey;
    DWORD       Value;
    DWORD       ValueLength;
    DWORD       Type;
    HRESULT     Error;

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         SERVICE_KEY(XENVBD),
                         0,
                         KEY_ALL_ACCESS,
                         &ServiceKey);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    ValueLength = sizeof (DWORD);

    Error = RegQueryValueEx(ServiceKey,
                            "Count",
                            NULL,
                            &Type,
                            (LPBYTE)&Value,
                            &ValueLength);
    if (Error != ERROR_SUCCESS) {
        if (Error == ERROR_FILE_NOT_FOUND) {
            Value = 0;
            goto done;
        }

        SetLastError(Error);
        goto fail2;
    }

    if (Type != REG_DWORD) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail3;
    }

done:
    Value++;

    Error = RegSetValueEx(ServiceKey,
                          "Count",
                          0,
                          REG_DWORD,
                          (LPBYTE)&Value,
                          ValueLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail4;
    }

    *Count = Value;

    RegCloseKey(ServiceKey);

    return TRUE;

fail4:
    Log("fail4");

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    RegCloseKey(ServiceKey);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
DecrementServiceCount(
    OUT PDWORD  Count
    )
{
    HKEY        ServiceKey;
    DWORD       Value;
    DWORD       ValueLength;
    DWORD       Type;
    HRESULT     Error;

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         SERVICE_KEY(XENVBD),
                         0,
                         KEY_ALL_ACCESS,
                         &ServiceKey);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    ValueLength = sizeof (DWORD);

    Error = RegQueryValueEx(ServiceKey,
                            "Count",
                            NULL,
                            &Type,
                            (LPBYTE)&Value,
                            &ValueLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail2;
    }

    if (Type != REG_DWORD) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail3;
    }

    if (Value == 0) {
        SetLastError(ERROR_INVALID_DATA);
        goto fail4;
    }

    --Value;

    Error = RegSetValueEx(ServiceKey,
                          "Count",
                          0,
                          REG_DWORD,
                          (LPBYTE)&Value,
                          ValueLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail5;
    }

    *Count = Value;

    RegCloseKey(ServiceKey);

    return TRUE;

fail5:
    Log("fail5");

fail4:
    Log("fail4");

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    RegCloseKey(ServiceKey);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
RequestReboot(
    IN  HDEVINFO            DeviceInfoSet,
    IN  PSP_DEVINFO_DATA    DeviceInfoData
    )
{
    SP_DEVINSTALL_PARAMS    DeviceInstallParams;

    DeviceInstallParams.cbSize = sizeof (DeviceInstallParams);

    if (!SetupDiGetDeviceInstallParams(DeviceInfoSet,
                                       DeviceInfoData,
                                       &DeviceInstallParams))
        goto fail1;

    DeviceInstallParams.Flags |= DI_NEEDREBOOT;

    Log("Flags = %08x", DeviceInstallParams.Flags);

    if (!SetupDiSetDeviceInstallParams(DeviceInfoSet,
                                       DeviceInfoData,
                                       &DeviceInstallParams))
        goto fail2;

    return TRUE;

fail2:
fail1:
    return FALSE;
}

static FORCEINLINE HRESULT
__DifInstallPreProcess(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    Log("====>");

    UNREFERENCED_PARAMETER(DeviceInfoSet);
    UNREFERENCED_PARAMETER(DeviceInfoData);
    UNREFERENCED_PARAMETER(Context);

    Log("<====");

    return ERROR_DI_POSTPROCESSING_REQUIRED; 
}

static FORCEINLINE HRESULT
__DifInstallPostProcess(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    BOOLEAN                         Success;
    DWORD                           Count;
    HRESULT                         Error;

    UNREFERENCED_PARAMETER(DeviceInfoSet);
    UNREFERENCED_PARAMETER(DeviceInfoData);

    Log("====>");

    Error = Context->InstallResult;
    if (Error != NO_ERROR) {
        SetLastError(Error);
        goto fail1;
    }

    Success = RequestUnplug();
    if (!Success)
        goto fail2;

    Success = IncrementServiceCount(&Count);
    if (!Success)
        goto fail3;

    if (Count == 1) {
        ModifyBootGPO();
        (VOID) RequestReboot(DeviceInfoSet, DeviceInfoData);
    }

    return NO_ERROR;

fail3:
    Log("fail3");

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return Error;
}

static DECLSPEC_NOINLINE HRESULT
DifInstall(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    SP_DEVINSTALL_PARAMS            DeviceInstallParams;
    HRESULT                         Error;

    DeviceInstallParams.cbSize = sizeof (DeviceInstallParams);

    if (!SetupDiGetDeviceInstallParams(DeviceInfoSet,
                                       DeviceInfoData,
                                       &DeviceInstallParams))
        goto fail1;

    Log("Flags = %08x", DeviceInstallParams.Flags);

    Error = (!Context->PostProcessing) ?
            __DifInstallPreProcess(DeviceInfoSet, DeviceInfoData, Context) :
            __DifInstallPostProcess(DeviceInfoSet, DeviceInfoData, Context);

    return Error;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return Error;
}

static FORCEINLINE HRESULT
__DifRemovePreProcess(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    BOOLEAN                         Success;
    DWORD                           Count;
    HRESULT                         Error;

    UNREFERENCED_PARAMETER(DeviceInfoSet);
    UNREFERENCED_PARAMETER(DeviceInfoData);
    UNREFERENCED_PARAMETER(Context);

    Log("====>");

    Success = DecrementServiceCount(&Count);
    if (!Success)
        goto fail1;    

    Context->PrivateData = (Count == 0) ? (PVOID)TRUE : (PVOID)FALSE;

    Log("<====");

    return ERROR_DI_POSTPROCESSING_REQUIRED; 

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return Error;
}

static FORCEINLINE HRESULT
__DifRemovePostProcess(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    BOOLEAN                         NeedReboot;
    BOOLEAN                         Success;
    HRESULT                         Error;

    Log("====>");

    Error = Context->InstallResult;
    if (Error != NO_ERROR) {
        SetLastError(Error);
        goto fail1;
    }

    NeedReboot = (BOOLEAN)(ULONG_PTR)Context->PrivateData;

    if (!NeedReboot)
        goto done;

    Success = RequestReboot(DeviceInfoSet, DeviceInfoData);
    if (!Success)
        goto fail2;

done:
    Log("<====");

    return NO_ERROR;

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return Error;
}

static DECLSPEC_NOINLINE HRESULT
DifRemove(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    SP_DEVINSTALL_PARAMS            DeviceInstallParams;
    HRESULT                         Error;

    DeviceInstallParams.cbSize = sizeof (DeviceInstallParams);

    if (!SetupDiGetDeviceInstallParams(DeviceInfoSet,
                                       DeviceInfoData,
                                       &DeviceInstallParams))
        goto fail1;

    Log("Flags = %08x", DeviceInstallParams.Flags);

    Error = (!Context->PostProcessing) ?
            __DifRemovePreProcess(DeviceInfoSet, DeviceInfoData, Context) :
            __DifRemovePostProcess(DeviceInfoSet, DeviceInfoData, Context);

    return Error;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return Error;
}

DWORD CALLBACK
Entry(
    IN  DI_FUNCTION                 Function,
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    HRESULT                         Error;

    Log("%s (%s) ===>",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR);

    if (!Context->PostProcessing) {
        Log("%s PreProcessing",
            FunctionName(Function));
    } else {
        Log("%s PostProcessing (%08x)",
            FunctionName(Function),
            Context->InstallResult);
    }

    switch (Function) {
    case DIF_INSTALLDEVICE:
        Error = DifInstall(DeviceInfoSet, DeviceInfoData, Context);
        break;
    case DIF_REMOVE:
        Error = DifRemove(DeviceInfoSet, DeviceInfoData, Context);
        break;
    default:
        if (!Context->PostProcessing) {
            Error = NO_ERROR;
        } else {
            Error = Context->InstallResult;
        }

        break;
    }

    Log("%s (%s) <===",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR);

    return (DWORD)Error;
}

DWORD CALLBACK
Version(
    IN  HWND        Window,
    IN  HINSTANCE   Module,
    IN  PTCHAR      Buffer,
    IN  INT         Reserved
    )
{
    UNREFERENCED_PARAMETER(Window);
    UNREFERENCED_PARAMETER(Module);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Reserved);

    Log("%s (%s)",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR);

    return NO_ERROR;
}

static FORCEINLINE const CHAR *
__ReasonName(
    IN  DWORD       Reason
    )
{
#define _NAME(_Reason)          \
        case DLL_ ## _Reason:   \
            return #_Reason;

    switch (Reason) {
    _NAME(PROCESS_ATTACH);
    _NAME(PROCESS_DETACH);
    _NAME(THREAD_ATTACH);
    _NAME(THREAD_DETACH);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _NAME
}

BOOL WINAPI
DllMain(
    IN  HINSTANCE   Module,
    IN  DWORD       Reason,
    IN  PVOID       Reserved
    )
{
    UNREFERENCED_PARAMETER(Module);
    UNREFERENCED_PARAMETER(Reserved);

    Log("%s (%s): %s",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR,
        __ReasonName(Reason));

    return TRUE;
}
