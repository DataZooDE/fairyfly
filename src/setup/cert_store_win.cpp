// LocalMachine\My certificate lookup, validation data and export: native CryptoAPI. Only certificate creation and
// removal (and the firewall, see firewall_win.cpp) go through the constant PowerShell scripts.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "include/setup/setup_hosts.h"
#include "include/setup/setup_validators.h"
#include "include/system/cert_scripts.h"
#include "include/system/powershell_runner.h"
#include "win_util.h"

namespace fairyfly::setup {

namespace {

using namespace win;

struct StoreHandle {
    HCERTSTORE handle = nullptr;
    StoreHandle() {
        handle = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                               CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG,
                               L"MY");
        if (!handle) throw_win(GetLastError(), "open LocalMachine\\My");
    }
    ~StoreHandle() {
        if (handle) CertCloseStore(handle, 0);
    }
};

long long filetime_to_unix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<long long>((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

std::string thumbprint_of(PCCERT_CONTEXT ctx) {
    BYTE hash[20];
    DWORD size = sizeof(hash);
    if (!CertGetCertificateContextProperty(ctx, CERT_SHA1_HASH_PROP_ID, hash, &size)) return {};
    return hex_upper(hash, size);
}

CertInfo describe(PCCERT_CONTEXT ctx) {
    CertInfo c;
    c.found = true;
    c.thumbprint = thumbprint_of(ctx);
    DWORD n = CertGetNameStringW(ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
    std::wstring subject(n ? n - 1 : 0, L'\0');
    if (n > 1) CertGetNameStringW(ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, subject.data(), n);
    c.subject = "CN=" + narrow(subject);
    DWORD size = 0;
    if (CertGetCertificateContextProperty(ctx, CERT_FRIENDLY_NAME_PROP_ID, nullptr, &size) && size > 0) {
        std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
        if (CertGetCertificateContextProperty(ctx, CERT_FRIENDLY_NAME_PROP_ID, buffer.data(), &size)) c.friendly_name = narrow(buffer.data());
    }
    size = 0;
    c.has_private_key = CertGetCertificateContextProperty(ctx, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size) && size > 0;
    c.not_after = filetime_to_unix(ctx->pCertInfo->NotAfter);
    // SAN DNS names; the CN only when there is no SAN extension.
    PCERT_EXTENSION ext = CertFindExtension(szOID_SUBJECT_ALT_NAME2, ctx->pCertInfo->cExtension, ctx->pCertInfo->rgExtension);
    if (ext) {
        PCERT_ALT_NAME_INFO info = nullptr;
        DWORD info_size = 0;
        if (CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME, ext->Value.pbData, ext->Value.cbData,
                                CRYPT_DECODE_ALLOC_FLAG, nullptr, &info, &info_size) && info) {
            for (DWORD i = 0; i < info->cAltEntry; ++i)
                if (info->rgAltEntry[i].dwAltNameChoice == CERT_ALT_NAME_DNS_NAME && info->rgAltEntry[i].pwszDNSName)
                    c.dns_names.push_back(narrow(info->rgAltEntry[i].pwszDNSName));
            LocalFree(info);
        }
    }
    if (c.dns_names.empty()) c.dns_names.push_back(narrow(subject));
    return c;
}

std::vector<unsigned char> encoded_of(PCCERT_CONTEXT ctx) {
    return std::vector<unsigned char>(ctx->pbCertEncoded, ctx->pbCertEncoded + ctx->cbCertEncoded);
}

std::string to_pem(const std::vector<unsigned char>& der) {
    DWORD size = 0;
    CryptBinaryToStringA(der.data(), static_cast<DWORD>(der.size()), CRYPT_STRING_BASE64HEADER, nullptr, &size);
    std::string pem(size, '\0');
    CryptBinaryToStringA(der.data(), static_cast<DWORD>(der.size()), CRYPT_STRING_BASE64HEADER, pem.data(), &size);
    pem.resize(size);
    return pem;
}

std::vector<unsigned char> render_file(const std::vector<unsigned char>& der, CerFormat format) {
    if (format == CerFormat::Der) return der;
    const std::string pem = to_pem(der);
    return std::vector<unsigned char>(pem.begin(), pem.end());
}

std::optional<std::vector<unsigned char>> read_bytes(const std::string& path) {
    std::ifstream in(win::to_path(path), std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class RealCertStore : public CertStore {
public:
    explicit RealCertStore(sys::PowerShellRunner& runner) : ps_(runner) {}

    CertInfo find_by_thumbprint(const std::string& thumbprint) override {
        if (thumbprint_error(thumbprint)) return {};
        StoreHandle store;
        const auto bytes = hex_to_bytes(normalize_thumbprint(thumbprint));
        CRYPT_HASH_BLOB blob{static_cast<DWORD>(bytes.size()), const_cast<BYTE*>(bytes.data())};
        PCCERT_CONTEXT ctx = CertFindCertificateInStore(store.handle, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &blob, nullptr);
        if (!ctx) return {};
        CertInfo info = describe(ctx);
        CertFreeCertificateContext(ctx);
        return info;
    }

    CertInfo find_self_signed(const std::string& hostname) override {
        StoreHandle store;
        const std::string wanted = "fairyfly-mcp " + hostname;
        CertInfo best;
        PCCERT_CONTEXT ctx = nullptr;
        while ((ctx = CertEnumCertificatesInStore(store.handle, ctx)) != nullptr) {
            CertInfo info = describe(ctx);
            if (info.friendly_name == wanted && (!best.found || info.not_after > best.not_after)) best = info;
        }
        return best;
    }

    CertInfo create_self_signed(const std::string& hostname, int valid_days) override {
        if (hostname_error(hostname)) throw HostError("INVALID_ARGUMENT", "invalid host name");
        const nlohmann::json j = run(sys::kCreateSelfSigned, sys::create_self_signed_params(hostname, valid_days));
        CertInfo c;
        c.found = j.value("found", false);
        c.thumbprint = j.value("thumbprint", "");
        c.subject = j.value("subject", "");
        c.friendly_name = "fairyfly-mcp " + hostname;
        for (const auto& n : j.value("dns_names", nlohmann::json::array()))
            if (n.is_string()) c.dns_names.push_back(n.get<std::string>());
        c.not_after = j.value("not_after", 0LL);
        c.has_private_key = j.value("has_private_key", false);
        return c;
    }

    CerFileState cer_file_state(const std::string& thumbprint, const std::string& path, CerFormat format) override {
        const auto existing = read_bytes(path);
        if (!existing) return CerFileState::Missing;
        StoreHandle store;
        const auto bytes = hex_to_bytes(normalize_thumbprint(thumbprint));
        CRYPT_HASH_BLOB blob{static_cast<DWORD>(bytes.size()), const_cast<BYTE*>(bytes.data())};
        PCCERT_CONTEXT ctx = CertFindCertificateInStore(store.handle, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &blob, nullptr);
        if (!ctx) return CerFileState::Differs;
        const bool same = *existing == render_file(encoded_of(ctx), format);
        CertFreeCertificateContext(ctx);
        return same ? CerFileState::Matches : CerFileState::Differs;
    }

    std::optional<std::string> file_thumbprint(const std::string& path) override {
        auto bytes = read_bytes(path);
        if (!bytes || bytes->empty() || bytes->size() > 1024 * 1024) return std::nullopt;
        // PEM text -> DER (CRYPT_STRING_BASE64_ANY also accepts bare base64; DER bytes are tried first)
        PCCERT_CONTEXT ctx = CertCreateCertificateContext(X509_ASN_ENCODING, bytes->data(), static_cast<DWORD>(bytes->size()));
        if (!ctx) {
            DWORD der_size = 0;
            if (!CryptStringToBinaryA(reinterpret_cast<const char*>(bytes->data()), static_cast<DWORD>(bytes->size()), CRYPT_STRING_BASE64HEADER, nullptr, &der_size, nullptr, nullptr) || der_size == 0)
                return std::nullopt;
            std::vector<unsigned char> der(der_size);
            if (!CryptStringToBinaryA(reinterpret_cast<const char*>(bytes->data()), static_cast<DWORD>(bytes->size()), CRYPT_STRING_BASE64HEADER, der.data(), &der_size, nullptr, nullptr))
                return std::nullopt;
            ctx = CertCreateCertificateContext(X509_ASN_ENCODING, der.data(), der_size);
            if (!ctx) return std::nullopt;
        }
        const std::string thumb = thumbprint_of(ctx);
        CertFreeCertificateContext(ctx);
        if (thumb.empty()) return std::nullopt;
        return thumb;
    }

    std::string export_cer(const std::string& thumbprint, const std::string& path, CerFormat format) override {
        const CerFileState state = cer_file_state(thumbprint, path, format);
        if (state == CerFileState::Matches) return "unchanged";
        StoreHandle store;
        const auto bytes = hex_to_bytes(normalize_thumbprint(thumbprint));
        CRYPT_HASH_BLOB blob{static_cast<DWORD>(bytes.size()), const_cast<BYTE*>(bytes.data())};
        PCCERT_CONTEXT ctx = CertFindCertificateInStore(store.handle, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &blob, nullptr);
        if (!ctx) throw HostError("NOT_FOUND", "certificate " + thumbprint + " is not in LocalMachine\\My");
        const auto data = render_file(encoded_of(ctx), format);
        CertFreeCertificateContext(ctx);
        std::error_code ec;
        const auto target = win::to_path(path);
        if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) throw HostError("HOST_ERROR", "cannot write " + path);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) throw HostError("HOST_ERROR", "cannot write " + path);
        return state == CerFileState::Missing ? "created" : "updated";
    }

    bool remove(const std::string& thumbprint) override {
        if (const auto e = thumbprint_error(thumbprint)) throw HostError("INVALID_ARGUMENT", *e);
        const nlohmann::json j = run(sys::kRemoveCert, sys::remove_cert_params(normalize_thumbprint(thumbprint)));
        return j.value("removed", false);
    }

private:
    nlohmann::json run(const std::string& script, const std::string& params) {
        try {
            return sys::run_json_script(ps_, script, params);
        } catch (const sys::PowerShellError& e) {
            throw HostError(e.code.empty() ? "PS_SCRIPT_FAILED" : e.code, e.message);
        }
    }
    sys::PowerShellRunner& ps_;
};

} // namespace

std::unique_ptr<CertStore> make_real_cert_store(sys::PowerShellRunner& runner) { return std::make_unique<RealCertStore>(runner); }

} // namespace fairyfly::setup
