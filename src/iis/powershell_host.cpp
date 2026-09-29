#include "include/iis/powershell_host.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "include/base64.h"
#include "include/iis/validators.h"

namespace fairyfly::iis {

using nlohmann::json;

namespace {

// ---------------------------------------------------------------------------------------------
// Constant scripts. Parameters are read from $env:FAIRYFLY_IIS_PARAMS (JSON); results are compact JSON.
// No script contains a placeholder that C++ fills in: the text is identical for every call.

const std::string kPreamble = R"PS(trap { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$p = $null
if ($env:FAIRYFLY_IIS_PARAMS) { $p = $env:FAIRYFLY_IIS_PARAMS | ConvertFrom-Json }
function Out-Json($o) { [Console]::Out.Write(($o | ConvertTo-Json -Compress -Depth 8)) }
function Cert-Json($c) {
  $names = @()
  if ($c.DnsNameList) { foreach ($n in $c.DnsNameList) { $names += $n.Unicode } }
  $cn = $c.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
  if ($cn) { $names += $cn }
  [ordered]@{
    thumbprint = $c.Thumbprint.ToUpper()
    subject = $c.Subject
    dns_names = @($names | Select-Object -Unique)
    not_after = [int64][DateTimeOffset]::new($c.NotAfter.ToUniversalTime()).ToUnixTimeSeconds()
    has_private_key = [bool]$c.HasPrivateKey
  }
}
)PS";

const std::string kDetect = kPreamble + R"PS(
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$elev = ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$inet = Join-Path $env:windir 'System32\inetsrv'
$iis = (Test-Path (Join-Path $inet 'w3wp.exe')) -or ($null -ne (Get-Service W3SVC -ErrorAction SilentlyContinue))
$mod = ''
if (Get-Module -ListAvailable -Name WebAdministration) { $mod = 'WebAdministration' }
elseif (Get-Module -ListAvailable -Name IISAdministration) { $mod = 'IISAdministration' }
$rw = Test-Path (Join-Path $inet 'rewrite.dll')
$arr = (Test-Path (Join-Path $inet 'requestRouter.dll')) -or (Test-Path (Join-Path $env:ProgramFiles 'IIS\Application Request Routing\requestRouter.dll'))
$ipsec = Test-Path (Join-Path $inet 'iprestriction.dll')
$proxy = $false
if ($iis -and $arr -and $mod -eq 'WebAdministration') {
  try {
    Import-Module WebAdministration
    $v = Get-WebConfigurationProperty -PSPath 'MACHINE/WEBROOT/APPHOST' -Filter 'system.webServer/proxy' -Name enabled
    if ($v -is [bool]) { $proxy = $v } else { $proxy = [bool]$v.Value }
  } catch { $proxy = $false }
}
Out-Json ([ordered]@{ elevated = [bool]$elev; iis_installed = [bool]$iis; admin_module = $mod; url_rewrite = [bool]$rw; arr_installed = [bool]$arr; arr_proxy_enabled = [bool]$proxy; ip_security = [bool]$ipsec })
)PS";

const std::string kGetSite = kPreamble + R"PS(
Import-Module WebAdministration
$s = Get-Website -Name $p.name
if ($null -eq $s) { Out-Json ([ordered]@{ exists = $false }) }
else {
  $bs = @()
  foreach ($b in $s.bindings.Collection) {
    $bi = ([string]$b.bindingInformation) -split ':', 3
    $bs += [ordered]@{ protocol = [string]$b.protocol; host = [string]$bi[2]; port = [int]$bi[1]; thumbprint = ([string]$b.certificateHash).ToUpper() }
  }
  Out-Json ([ordered]@{ exists = $true; state = [string]$s.State; physical_path = [string]$s.physicalPath; app_pool = [string]$s.applicationPool; bindings = @($bs) })
}
)PS";

const std::string kGetAppPool = kPreamble + R"PS(
Import-Module WebAdministration
$pp = 'IIS:\AppPools\' + $p.name
if (-not (Test-Path -LiteralPath $pp)) { Out-Json ([ordered]@{ exists = $false }) }
else {
  $i = Get-Item -LiteralPath $pp
  Out-Json ([ordered]@{ exists = $true; runtime_version = [string]$i.managedRuntimeVersion; identity = [string]$i.processModel.identityType; state = [string]$i.state })
}
)PS";

const std::string kEnsureAppPool = kPreamble + R"PS(
Import-Module WebAdministration
$pp = 'IIS:\AppPools\' + $p.name
$change = 'unchanged'
if (-not (Test-Path -LiteralPath $pp)) { New-WebAppPool -Name $p.name | Out-Null; $change = 'created' }
$i = Get-Item -LiteralPath $pp
if ([string]$i.managedRuntimeVersion -ne '') {
  Set-ItemProperty -LiteralPath $pp -Name managedRuntimeVersion -Value ''
  if ($change -eq 'unchanged') { $change = 'updated' }
}
if (([string]$i.processModel.identityType) -notin @('ApplicationPoolIdentity', '4')) {
  Set-ItemProperty -LiteralPath $pp -Name processModel.identityType -Value 'ApplicationPoolIdentity'
  if ($change -eq 'unchanged') { $change = 'updated' }
}
Out-Json ([ordered]@{ change = $change })
)PS";

const std::string kEnsureSite = kPreamble + R"PS(
Import-Module WebAdministration
$change = 'unchanged'
$s = Get-Website -Name $p.name
if ($null -eq $s) {
  New-Website -Name $p.name -PhysicalPath $p.physical_path -ApplicationPool $p.app_pool -Port $p.port -HostHeader $p.hostname -Ssl -SslFlags 1 | Out-Null
  $change = 'created'
} else {
  $sp = 'IIS:\Sites\' + $p.name
  if ([string]$s.physicalPath -ne $p.physical_path) { Set-ItemProperty -LiteralPath $sp -Name physicalPath -Value $p.physical_path; $change = 'updated' }
  if ([string]$s.applicationPool -ne $p.app_pool) { Set-ItemProperty -LiteralPath $sp -Name applicationPool -Value $p.app_pool; $change = 'updated' }
}
$b = Get-WebBinding -Name $p.name -Protocol https -Port $p.port -HostHeader $p.hostname
if ($null -eq $b) {
  New-WebBinding -Name $p.name -Protocol https -Port $p.port -HostHeader $p.hostname -SslFlags 1
  $b = Get-WebBinding -Name $p.name -Protocol https -Port $p.port -HostHeader $p.hostname
  if ($change -eq 'unchanged') { $change = 'updated' }
}
if (([string]$b.certificateHash) -ne $p.thumbprint) {
  if ($b.certificateHash) { $b.RemoveSslCertificate() }
  $b.AddSslCertificate($p.thumbprint, 'my')
  if ($change -eq 'unchanged') { $change = 'updated' }
}
$s = Get-Website -Name $p.name
if ([string]$s.State -ne 'Started') {
  Start-Website -Name $p.name
  if ($change -eq 'unchanged') { $change = 'updated' }
}
Out-Json ([ordered]@{ change = $change })
)PS";

const std::string kConfigAccess = kPreamble + R"PS(
Import-Module WebAdministration
$apphost = 'MACHINE/WEBROOT/APPHOST'
$change = 'unchanged'
foreach ($v in $p.variables) {
  $f = "system.webServer/rewrite/allowedServerVariables/add[@name='" + $v + "']"
  $existing = Get-WebConfigurationProperty -PSPath $apphost -Location $p.site_name -Filter $f -Name name -ErrorAction SilentlyContinue
  if ($null -eq $existing) {
    Add-WebConfigurationProperty -PSPath $apphost -Location $p.site_name -Filter 'system.webServer/rewrite/allowedServerVariables' -Name '.' -Value @{ name = $v }
    $change = 'updated'
  }
}
foreach ($sec in @('system.webServer/security/ipSecurity', 'system.webServer/proxy')) {
  Set-WebConfiguration -PSPath $apphost -Location $p.site_name -Filter ('//' + $sec) -Metadata overrideMode -Value Allow
}
Out-Json ([ordered]@{ change = $change })
)PS";

const std::string kRestrictAcl = kPreamble + R"PS(
$acl = Get-Acl -LiteralPath $p.path
$pool = 'IIS AppPool\' + $p.app_pool
$want = @($pool, 'BUILTIN\Administrators', 'NT AUTHORITY\SYSTEM') | ForEach-Object { $_.ToLower() } | Sort-Object
$have = @($acl.Access | ForEach-Object { $_.IdentityReference.Value.ToLower() } | Sort-Object -Unique)
if ($acl.AreAccessRulesProtected -and (($have -join '|') -eq ($want -join '|'))) { Out-Json ([ordered]@{ change = 'unchanged' }) }
else {
  $acl.SetAccessRuleProtection($true, $false)
  foreach ($r in @($acl.Access)) { [void]$acl.RemoveAccessRule($r) }
  $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($pool, 'Read', 'Allow')))
  $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule('BUILTIN\Administrators', 'FullControl', 'Allow')))
  $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule('NT AUTHORITY\SYSTEM', 'FullControl', 'Allow')))
  Set-Acl -LiteralPath $p.path -AclObject $acl
  Out-Json ([ordered]@{ change = 'updated' })
}
)PS";

const std::string kFindCert = kPreamble + R"PS(
$c = Get-Item -LiteralPath ('Cert:\LocalMachine\My\' + $p.thumbprint) -ErrorAction SilentlyContinue
if ($null -eq $c) { Out-Json ([ordered]@{ found = $false }) }
else { $o = Cert-Json $c; $o['found'] = $true; Out-Json $o }
)PS";

const std::string kFindSelfSigned = kPreamble + R"PS(
$c = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.FriendlyName -eq ('fairyfly-mcp ' + $p.hostname) } | Sort-Object NotAfter -Descending | Select-Object -First 1
if ($null -eq $c) { Out-Json ([ordered]@{ found = $false }) }
else { $o = Cert-Json $c; $o['found'] = $true; Out-Json $o }
)PS";

const std::string kCreateSelfSigned = kPreamble + R"PS(
$c = New-SelfSignedCertificate -Subject ('CN=' + $p.hostname) -DnsName $p.hostname -CertStoreLocation 'Cert:\LocalMachine\My' `
  -FriendlyName ('fairyfly-mcp ' + $p.hostname) -NotAfter (Get-Date).AddDays([int]$p.valid_days) `
  -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyUsage DigitalSignature, KeyEncipherment `
  -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.1')
$o = Cert-Json $c
$o['found'] = $true
Out-Json $o
)PS";

const std::string kExportCert = kPreamble + R"PS(
$c = Get-Item -LiteralPath ('Cert:\LocalMachine\My\' + $p.thumbprint)
$change = 'created'
if (Test-Path -LiteralPath $p.path) {
  $existing = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList $p.path
  if ($existing.Thumbprint -eq $c.Thumbprint) { $change = 'unchanged' } else { $change = 'updated' }
}
if ($change -ne 'unchanged') { Export-Certificate -Cert $c -FilePath $p.path -Type CERT -Force | Out-Null }
Out-Json ([ordered]@{ change = $change })
)PS";

const std::string kRemoveCert = kPreamble + R"PS(
$path = 'Cert:\LocalMachine\My\' + $p.thumbprint
if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -DeleteKey; Out-Json ([ordered]@{ removed = $true }) }
else { Out-Json ([ordered]@{ removed = $false }) }
)PS";

const std::string kFirewallExists = kPreamble + R"PS(
$r = Get-NetFirewallRule -DisplayName $p.rule_name -ErrorAction SilentlyContinue
Out-Json ([ordered]@{ exists = ($null -ne $r) })
)PS";

const std::string kFirewallEnsure = kPreamble + R"PS(
$r = Get-NetFirewallRule -DisplayName $p.rule_name -ErrorAction SilentlyContinue
if ($null -eq $r) {
  New-NetFirewallRule -DisplayName $p.rule_name -Direction Inbound -Action Allow -Protocol TCP -LocalPort ([int]$p.port) -Profile Any | Out-Null
  Out-Json ([ordered]@{ change = 'created' })
} else {
  $pf = $r | Get-NetFirewallPortFilter
  if ([string]$pf.LocalPort -eq [string]$p.port) { Out-Json ([ordered]@{ change = 'unchanged' }) }
  else { $pf | Set-NetFirewallPortFilter -LocalPort ([int]$p.port); Out-Json ([ordered]@{ change = 'updated' }) }
}
)PS";

const std::string kFirewallRemove = kPreamble + R"PS(
$r = Get-NetFirewallRule -DisplayName $p.rule_name -ErrorAction SilentlyContinue
if ($null -eq $r) { Out-Json ([ordered]@{ removed = $false }) }
else { $r | Remove-NetFirewallRule; Out-Json ([ordered]@{ removed = $true }) }
)PS";

const std::string kRemoveSite = kPreamble + R"PS(
Import-Module WebAdministration
$s = Get-Website -Name $p.name
if ($null -eq $s) { Out-Json ([ordered]@{ removed = $false }) }
else {
  if ([string]$s.State -eq 'Started') { Stop-Website -Name $p.name }
  Remove-Website -Name $p.name
  try { Clear-WebConfiguration -PSPath 'MACHINE/WEBROOT/APPHOST' -Location $p.name -Filter 'system.webServer/rewrite/allowedServerVariables' } catch {}
  Out-Json ([ordered]@{ removed = $true })
}
)PS";

const std::string kRemoveAppPool = kPreamble + R"PS(
Import-Module WebAdministration
$pp = 'IIS:\AppPools\' + $p.name
if (Test-Path -LiteralPath $pp) { Remove-WebAppPool -Name $p.name; Out-Json ([ordered]@{ removed = $true }) }
else { Out-Json ([ordered]@{ removed = $false }) }
)PS";

// ---------------------------------------------------------------------------------------------

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::filesystem::path fs_path(const std::string& utf8) { return std::filesystem::path(widen(utf8)); }

class WindowsPowerShellRunner final : public PowerShellRunner {
public:
    PsResult run(const std::string& script, const std::string& params_json, int timeout_ms) override {
        PsResult result;
        const std::wstring wide = widen(script);
        std::vector<uint8_t> bytes(wide.size() * 2);
        std::memcpy(bytes.data(), wide.data(), bytes.size());
        const std::wstring encoded = widen(utils::base64_encode(bytes));

        wchar_t sysdir[MAX_PATH] = {};
        GetSystemDirectoryW(sysdir, MAX_PATH);
        const std::wstring exe = std::wstring(sysdir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        std::wstring cmd = L"\"" + exe + L"\" -NoLogo -NoProfile -NonInteractive -OutputFormat Text -ExecutionPolicy Bypass -EncodedCommand " + encoded;

        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
        if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0)) {
            throw HostError{"IIS_SCRIPT_FAILED", "could not create pipes for PowerShell"};
        }
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = out_w;
        si.hStdError = err_w;
        PROCESS_INFORMATION pi{};

        const std::wstring env_name = widen(kParamsEnvVar);
        SetEnvironmentVariableW(env_name.c_str(), widen(params_json).c_str());
        const BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        SetEnvironmentVariableW(env_name.c_str(), nullptr);
        CloseHandle(out_w);
        CloseHandle(err_w);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!created) {
            CloseHandle(out_r);
            CloseHandle(err_r);
            throw HostError{"IIS_SCRIPT_FAILED", "could not start powershell.exe"};
        }

        auto drain = [](HANDLE h, std::string& into) {
            DWORD avail = 0;
            while (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                char buf[4096];
                DWORD got = 0;
                if (!ReadFile(h, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || got == 0) break;
                into.append(buf, got);
            }
        };
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
        for (;;) {
            drain(out_r, result.out);
            drain(err_r, result.err);
            if (WaitForSingleObject(pi.hProcess, 25) == WAIT_OBJECT_0) break;
            if (GetTickCount64() > deadline) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 2000);
                result.timed_out = true;
                break;
            }
        }
        drain(out_r, result.out);
        drain(err_r, result.err);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        result.exit_code = static_cast<int>(code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(out_r);
        CloseHandle(err_r);
        return result;
    }
};

std::string first_line(const std::string& s) {
    std::string line = s.substr(0, s.find_first_of("\r\n"));
    if (line.size() > 300) line.resize(300);
    return line;
}

Change parse_change(const std::string& text) {
    if (text == "created") return Change::Created;
    if (text == "updated") return Change::Updated;
    return Change::Unchanged;
}

CertInfo parse_cert(const json& j) {
    CertInfo c;
    c.thumbprint = j.value("thumbprint", "");
    c.subject = j.value("subject", "");
    for (const auto& n : j.value("dns_names", json::array())) c.dns_names.push_back(n.get<std::string>());
    c.not_after = j.value("not_after", static_cast<int64_t>(0));
    c.has_private_key = j.value("has_private_key", false);
    return c;
}

class PowerShellHost final : public IisHost {
public:
    explicit PowerShellHost(PowerShellRunner& runner) : runner_(runner) {}

    HostFacts detect() override {
        const json j = call(kDetect, json::object());
        HostFacts f;
        f.elevated = j.value("elevated", false);
        f.iis_installed = j.value("iis_installed", false);
        f.admin_module = j.value("admin_module", "");
        f.url_rewrite = j.value("url_rewrite", false);
        f.arr_installed = j.value("arr_installed", false);
        f.arr_proxy_enabled = j.value("arr_proxy_enabled", false);
        f.ip_security = j.value("ip_security", false);
        return f;
    }

    SiteInfo get_site(const std::string& name) override {
        const json j = call(kGetSite, json{{"name", name}});
        SiteInfo s;
        s.exists = j.value("exists", false);
        if (!s.exists) return s;
        s.state = j.value("state", "");
        s.physical_path = j.value("physical_path", "");
        s.app_pool = j.value("app_pool", "");
        for (const auto& b : j.value("bindings", json::array())) {
            s.bindings.push_back({b.value("protocol", ""), b.value("host", ""), b.value("port", 0), b.value("thumbprint", "")});
        }
        return s;
    }

    AppPoolInfo get_app_pool(const std::string& name) override {
        const json j = call(kGetAppPool, json{{"name", name}});
        AppPoolInfo a;
        a.exists = j.value("exists", false);
        a.runtime_version = j.value("runtime_version", "");
        a.identity = j.value("identity", "");
        a.state = j.value("state", "");
        return a;
    }

    std::optional<CertInfo> find_certificate(const std::string& thumbprint) override {
        const json j = call(kFindCert, json{{"thumbprint", thumbprint}});
        if (!j.value("found", false)) return std::nullopt;
        return parse_cert(j);
    }

    std::optional<CertInfo> find_self_signed(const std::string& hostname) override {
        const json j = call(kFindSelfSigned, json{{"hostname", hostname}});
        if (!j.value("found", false)) return std::nullopt;
        return parse_cert(j);
    }

    bool firewall_rule_exists(const std::string& rule) override {
        return call(kFirewallExists, json{{"rule_name", rule}}).value("exists", false);
    }

    bool tcp_reachable(const std::string& host, int port, int timeout_ms) override {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        bool ok = false;
        std::string h = host;
        if (!h.empty() && h.front() == '[') h = h.substr(1, h.size() - 2);
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICHOST;
        addrinfo* res = nullptr;
        if (getaddrinfo(h.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res) {
            SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (s != INVALID_SOCKET) {
                u_long nonblocking = 1;
                ioctlsocket(s, FIONBIO, &nonblocking);
                const int rc = connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
                if (rc == 0) {
                    ok = true;
                } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    fd_set writable, failed;
                    FD_ZERO(&writable);
                    FD_ZERO(&failed);
                    FD_SET(s, &writable);
                    FD_SET(s, &failed);
                    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
                    ok = select(0, nullptr, &writable, &failed, &tv) > 0 && FD_ISSET(s, &writable) && !FD_ISSET(s, &failed);
                }
                closesocket(s);
            }
            freeaddrinfo(res);
        }
        WSACleanup();
        return ok;
    }

    bool dir_exists(const std::string& path) override {
        std::error_code ec;
        return std::filesystem::is_directory(fs_path(path), ec);
    }

    std::optional<std::string> read_file(const std::string& path) override {
        std::ifstream in(fs_path(path), std::ios::binary);
        if (!in) return std::nullopt;
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    Change create_dir(const std::string& path) override {
        std::error_code ec;
        const auto p = fs_path(path);
        if (std::filesystem::is_directory(p, ec)) return Change::Unchanged;
        std::filesystem::create_directories(p, ec);
        if (ec) throw HostError{"IIS_FILE_FAILED", "could not create directory " + path};
        return Change::Created;
    }

    Change write_file(const std::string& path, const std::string& content) override {
        const auto existing = read_file(path);
        if (existing && *existing == content) return Change::Unchanged;
        std::ofstream out(fs_path(path), std::ios::binary | std::ios::trunc);
        if (!out) throw HostError{"IIS_FILE_FAILED", "could not write " + path};
        out << content;
        out.close();
        if (!out) throw HostError{"IIS_FILE_FAILED", "could not write " + path};
        return existing ? Change::Updated : Change::Created;
    }

    bool remove_file(const std::string& path) override {
        std::error_code ec;
        return std::filesystem::remove(fs_path(path), ec) && !ec;
    }

    bool remove_dir_if_empty(const std::string& path) override {
        std::error_code ec;
        const auto p = fs_path(path);
        if (!std::filesystem::is_directory(p, ec) || !std::filesystem::is_empty(p, ec)) return false;
        return std::filesystem::remove(p, ec) && !ec;
    }

    CertInfo create_self_signed(const std::string& hostname, int valid_days) override {
        return parse_cert(call(kCreateSelfSigned, json{{"hostname", hostname}, {"valid_days", valid_days}}));
    }

    Change export_certificate(const std::string& thumbprint, const std::string& cer_path) override {
        return parse_change(call(kExportCert, json{{"thumbprint", thumbprint}, {"path", cer_path}}).value("change", ""));
    }

    Change ensure_app_pool(const std::string& name) override {
        return parse_change(call(kEnsureAppPool, json{{"name", name}}).value("change", ""));
    }

    Change ensure_site(const SiteSpec& spec) override {
        return parse_change(call(kEnsureSite, json{{"name", spec.name}, {"physical_path", spec.physical_path},
                                                   {"app_pool", spec.app_pool}, {"hostname", spec.hostname},
                                                   {"port", spec.port}, {"thumbprint", spec.thumbprint}})
                                .value("change", ""));
    }

    Change ensure_site_config_access(const std::string& site_name, const std::vector<std::string>& variables) override {
        return parse_change(call(kConfigAccess, json{{"site_name", site_name}, {"variables", variables}}).value("change", ""));
    }

    Change restrict_acl(const std::string& path, const std::string& app_pool) override {
        return parse_change(call(kRestrictAcl, json{{"path", path}, {"app_pool", app_pool}}).value("change", ""));
    }

    Change ensure_firewall_rule(const std::string& rule, int port) override {
        return parse_change(call(kFirewallEnsure, json{{"rule_name", rule}, {"port", port}}).value("change", ""));
    }

    bool remove_firewall_rule(const std::string& rule) override {
        return call(kFirewallRemove, json{{"rule_name", rule}}).value("removed", false);
    }
    bool remove_site(const std::string& name) override { return call(kRemoveSite, json{{"name", name}}).value("removed", false); }
    bool remove_app_pool(const std::string& name) override { return call(kRemoveAppPool, json{{"name", name}}).value("removed", false); }
    bool remove_certificate(const std::string& thumbprint) override {
        return call(kRemoveCert, json{{"thumbprint", thumbprint}}).value("removed", false);
    }

private:
    json call(const std::string& script, const json& params) {
        const PsResult r = runner_.run(script, params.dump(), kScriptTimeoutMs);
        if (r.timed_out) throw HostError{"IIS_SCRIPT_TIMEOUT", "PowerShell did not finish within " + std::to_string(kScriptTimeoutMs / 1000) + " s"};
        if (r.exit_code != 0) throw HostError{"IIS_SCRIPT_FAILED", "PowerShell failed: " + first_line(r.err)};
        const json parsed = json::parse(r.out, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) throw HostError{"IIS_SCRIPT_FAILED", "PowerShell returned no JSON result"};
        return parsed;
    }

    PowerShellRunner& runner_;
};

} // namespace

std::unique_ptr<PowerShellRunner> make_windows_powershell_runner() { return std::make_unique<WindowsPowerShellRunner>(); }

std::unique_ptr<IisHost> make_powershell_host(PowerShellRunner& runner) { return std::make_unique<PowerShellHost>(runner); }

const std::vector<std::pair<std::string, std::string>>& script_catalog() {
    static const std::vector<std::pair<std::string, std::string>> catalog = {
        {"detect", kDetect},           {"get_site", kGetSite},           {"get_app_pool", kGetAppPool},
        {"ensure_app_pool", kEnsureAppPool}, {"ensure_site", kEnsureSite}, {"config_access", kConfigAccess},
        {"restrict_acl", kRestrictAcl}, {"find_cert", kFindCert},         {"find_self_signed", kFindSelfSigned},
        {"create_self_signed", kCreateSelfSigned}, {"export_cert", kExportCert}, {"remove_cert", kRemoveCert},
        {"firewall_exists", kFirewallExists}, {"firewall_ensure", kFirewallEnsure}, {"firewall_remove", kFirewallRemove},
        {"remove_site", kRemoveSite},  {"remove_app_pool", kRemoveAppPool},
    };
    return catalog;
}

} // namespace fairyfly::iis
