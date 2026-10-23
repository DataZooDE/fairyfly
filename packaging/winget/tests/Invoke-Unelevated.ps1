<#
.SYNOPSIS
  Runs a command line for the current user WITHOUT administrator rights and waits for it (CI helper).
.DESCRIPTION
  GitHub-hosted runners execute steps as an elevated administrator. To prove that the per-user installer needs no
  admin rights, the command runs under a "Basic User" token of the SAME account, computed with the Windows SAFER API
  (what `runas /trustlevel:0x20000` does): the Administrators group becomes deny-only, admin privileges are removed
  and the integrity level is set to Medium. No account is created and no password is used.
  stdout and stderr go to -OutFile; the exit code of the command is returned (and printed).
.EXAMPLE
  ./Invoke-Unelevated.ps1 -CommandLine 'whoami /groups' -OutFile out.txt
#>
param(
    [Parameter(Mandatory)][string]$CommandLine,
    [Parameter(Mandatory)][string]$OutFile,
    [int]$TimeoutSeconds = 600
)
$ErrorActionPreference = 'Stop'

if (-not ('Ffy.Unelevated' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace Ffy {
public static class Unelevated {
    const uint SAFER_SCOPEID_USER = 2, SAFER_LEVELID_NORMALUSER = 0x20000, SAFER_LEVEL_OPEN = 1;
    const int TokenIntegrityLevel = 25;
    const uint SE_GROUP_INTEGRITY = 0x20, CREATE_NO_WINDOW = 0x08000000, CREATE_UNICODE_ENVIRONMENT = 0x400;
    const uint INFINITE = 0xFFFFFFFF, WAIT_TIMEOUT = 0x102;

    [StructLayout(LayoutKind.Sequential)] struct SID_AND_ATTRIBUTES { public IntPtr Sid; public uint Attributes; }
    [StructLayout(LayoutKind.Sequential)] struct TOKEN_MANDATORY_LABEL { public SID_AND_ATTRIBUTES Label; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct STARTUPINFO {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
    [StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

    [DllImport("advapi32.dll", SetLastError = true)] static extern bool SaferCreateLevel(uint scope, uint level, uint flags, out IntPtr handle, IntPtr reserved);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool SaferComputeTokenFromLevel(IntPtr level, IntPtr inToken, out IntPtr outToken, uint flags, IntPtr reserved);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool SaferCloseLevel(IntPtr level);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool ConvertStringSidToSid(string sid, out IntPtr psid);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool SetTokenInformation(IntPtr token, int cls, ref TOKEN_MANDATORY_LABEL info, int len);
    [DllImport("advapi32.dll", SetLastError = true)] static extern int GetLengthSid(IntPtr sid);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool CreateProcessAsUser(IntPtr token, string app, string cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool CreateProcessWithTokenW(IntPtr token, uint logonFlags, string app, string cmd, uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetExitCodeProcess(IntPtr h, out uint code);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateProcess(IntPtr h, uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr h);

    public static int Run(string commandLine, string cwd, int timeoutSeconds) {
        IntPtr level, token, medium;
        if (!SaferCreateLevel(SAFER_SCOPEID_USER, SAFER_LEVELID_NORMALUSER, SAFER_LEVEL_OPEN, out level, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "SaferCreateLevel");
        try {
            if (!SaferComputeTokenFromLevel(level, IntPtr.Zero, out token, 0, IntPtr.Zero))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "SaferComputeTokenFromLevel");
        } finally { SaferCloseLevel(level); }
        if (!ConvertStringSidToSid("S-1-16-8192", out medium)) throw new Win32Exception(Marshal.GetLastWin32Error(), "ConvertStringSidToSid");
        var tml = new TOKEN_MANDATORY_LABEL();
        tml.Label.Sid = medium; tml.Label.Attributes = SE_GROUP_INTEGRITY;
        if (!SetTokenInformation(token, TokenIntegrityLevel, ref tml, Marshal.SizeOf(tml) + GetLengthSid(medium)))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "SetTokenInformation(Medium)");
        LocalFree(medium);

        var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(si); si.lpDesktop = "";
        PROCESS_INFORMATION pi;
        uint flags = CREATE_NO_WINDOW;
        if (!CreateProcessAsUser(token, null, commandLine, IntPtr.Zero, IntPtr.Zero, false, flags, IntPtr.Zero, cwd, ref si, out pi)) {
            int err = Marshal.GetLastWin32Error();
            // ERROR_PRIVILEGE_NOT_HELD: fall back to the secondary logon service (needs SeImpersonatePrivilege only)
            if (err != 1314 || !CreateProcessWithTokenW(token, 0, null, commandLine, flags, IntPtr.Zero, cwd, ref si, out pi))
                throw new Win32Exception(err == 1314 ? Marshal.GetLastWin32Error() : err, "CreateProcess with the basic-user token");
        }
        CloseHandle(token);
        try {
            uint ms = timeoutSeconds <= 0 ? INFINITE : (uint)timeoutSeconds * 1000;
            if (WaitForSingleObject(pi.hProcess, ms) == WAIT_TIMEOUT) { TerminateProcess(pi.hProcess, 1460); throw new TimeoutException(commandLine); }
            uint code; GetExitCodeProcess(pi.hProcess, out code);
            return unchecked((int)code);
        } finally { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    }
}
}
'@
}

$OutFile = [IO.Path]::GetFullPath($OutFile)
if (Test-Path $OutFile) { Remove-Item $OutFile -Force }
# cmd.exe does the redirection so the child needs no inherited handles
$wrapped = "cmd.exe /d /s /c `"$CommandLine > `"$OutFile`" 2>&1`""
$code = [Ffy.Unelevated]::Run($wrapped, (Get-Location).Path, $TimeoutSeconds)
if (Test-Path $OutFile) { Get-Content $OutFile | ForEach-Object { Write-Host "  | $_" } }
Write-Host "  (unelevated exit code $code = 0x$('{0:X8}' -f $code))"
return $code
