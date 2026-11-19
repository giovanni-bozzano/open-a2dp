#include "SetupSignature.h"

#include "SetupEnvironment.h"
#include "SetupNames.h"

#include <windows.h>

#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <vector>

namespace OpenA2dp::Setup {
namespace {

std::wstring HresultText(HRESULT result)
{
	wchar_t text[16] = {};
	swprintf_s(text, L"0x%08lX", static_cast<unsigned long>(result));
	return text;
}

HRESULT VerifyTrust(const std::wstring& file)
{
	WINTRUST_FILE_INFO fileInfo = {};
	fileInfo.cbStruct = sizeof(fileInfo);
	fileInfo.pcwszFilePath = file.c_str();

	WINTRUST_DATA data = {};
	data.cbStruct = sizeof(data);
	data.dwUIChoice = WTD_UI_NONE;
	data.fdwRevocationChecks = WTD_REVOKE_NONE;
	data.dwUnionChoice = WTD_CHOICE_FILE;
	data.pFile = &fileInfo;
	data.dwStateAction = WTD_STATEACTION_VERIFY;
	data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	const LONG status = WinVerifyTrust(nullptr, &action, &data);
	data.dwStateAction = WTD_STATEACTION_CLOSE;
	(void)WinVerifyTrust(nullptr, &action, &data);
	return static_cast<HRESULT>(status);
}

bool ExtractSigner(const std::wstring& file, PCCERT_CONTEXT& certificate, std::wstring& error)
{
	certificate = nullptr;
	HCERTSTORE store = nullptr;
	HCRYPTMSG message = nullptr;
	DWORD encoding = 0;
	DWORD contentType = 0;
	DWORD formatType = 0;
	if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, file.c_str(),
						  CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED |
							  CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED,
						  CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &contentType, &formatType, &store,
						  &message, nullptr))
	{
		error = L"The signature on " + file + L" could not be read (" +
				HresultText(static_cast<HRESULT>(GetLastError())) + L").";
		return false;
	}

	DWORD size = 0;
	if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &size) || size == 0)
	{
		error = L"The driver package has no signer information.";
		CryptMsgClose(message);
		CertCloseStore(store, 0);
		return false;
	}
	std::vector<BYTE> buffer(size);
	if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, buffer.data(), &size))
	{
		error = L"The driver package signer could not be read.";
		CryptMsgClose(message);
		CertCloseStore(store, 0);
		return false;
	}

	const auto* signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(buffer.data());
	CERT_INFO info = {};
	info.Issuer = signer->Issuer;
	info.SerialNumber = signer->SerialNumber;
	PCCERT_CONTEXT found = CertFindCertificateInStore(
		store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &info, nullptr);
	if (found != nullptr)
	{
		certificate = CertDuplicateCertificateContext(found);
		CertFreeCertificateContext(found);
	}
	CryptMsgClose(message);
	CertCloseStore(store, 0);
	if (certificate == nullptr)
	{
		error = L"The driver package signer is not in the signed file.";
		return false;
	}
	return true;
}

std::wstring CertificateName(PCCERT_CONTEXT certificate)
{
	const DWORD length =
		CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
	if (length <= 1)
	{
		return L"(unknown signer)";
	}
	std::wstring name(length, L'\0');
	CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name.data(), length);
	while (!name.empty() && name.back() == L'\0')
	{
		name.pop_back();
	}
	return name;
}

std::wstring CertificateThumbprint(PCCERT_CONTEXT certificate)
{
	DWORD size = 0;
	if (!CertGetCertificateContextProperty(certificate, CERT_SHA256_HASH_PROP_ID, nullptr, &size) ||
		size == 0)
	{
		return {};
	}
	std::vector<BYTE> hash(size);
	if (!CertGetCertificateContextProperty(certificate, CERT_SHA256_HASH_PROP_ID, hash.data(), &size))
	{
		return {};
	}
	std::wstring thumbprint;
	thumbprint.reserve(hash.size() * 2);
	for (const BYTE byte : hash)
	{
		wchar_t text[3] = {};
		swprintf_s(text, L"%02X", byte);
		thumbprint += text;
	}
	return thumbprint;
}

bool IsSelfSigned(PCCERT_CONTEXT certificate)
{
	return CertCompareCertificateName(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
									  &certificate->pCertInfo->Subject,
									  &certificate->pCertInfo->Issuer) != FALSE;
}

bool IsTestSigningEnabled()
{
	using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
	struct CodeIntegrityInformation
	{
		ULONG Length;
		ULONG Options;
	};
	constexpr ULONG kSystemCodeIntegrityInformation = 103;
	constexpr ULONG kTestSignOption = 0x40;

	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (ntdll == nullptr)
	{
		return false;
	}
	const auto query = reinterpret_cast<NtQuerySystemInformationFn>(
		GetProcAddress(ntdll, "NtQuerySystemInformation"));
	if (query == nullptr)
	{
		return false;
	}
	CodeIntegrityInformation information = {sizeof(information), 0};
	return query(kSystemCodeIntegrityInformation, &information, sizeof(information), nullptr) >= 0 &&
		   (information.Options & kTestSignOption) != 0;
}

bool AddToRoot(PCCERT_CONTEXT certificate, std::wstring& error)
{
	const HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
										   CERT_SYSTEM_STORE_LOCAL_MACHINE, L"ROOT");
	if (store == nullptr)
	{
		error = L"The machine Root store could not be opened (" +
				HresultText(static_cast<HRESULT>(GetLastError())) + L").";
		return false;
	}
	const BOOL added =
		CertAddCertificateContextToStore(store, certificate, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
	const DWORD code = GetLastError();
	CertCloseStore(store, 0);
	if (!added && code != CRYPT_E_EXISTS)
	{
		error = L"The driver certificate could not be trusted (" + HresultText(static_cast<HRESULT>(code)) +
				L").";
		return false;
	}
	return true;
}

bool RemoveFromRoot(const std::wstring& thumbprint)
{
	if (thumbprint.empty())
	{
		return true;
	}
	const HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
										   CERT_SYSTEM_STORE_LOCAL_MACHINE, L"ROOT");
	if (store == nullptr)
	{
		return false;
	}
	bool removed = false;
	PCCERT_CONTEXT certificate = nullptr;
	while ((certificate = CertEnumCertificatesInStore(store, certificate)) != nullptr)
	{
		if (CompareStringOrdinal(CertificateThumbprint(certificate).c_str(), -1, thumbprint.c_str(), -1,
								 TRUE) != CSTR_EQUAL)
		{
			continue;
		}
		PCCERT_CONTEXT duplicate = CertDuplicateCertificateContext(certificate);
		if (duplicate != nullptr)
		{
			removed = CertDeleteCertificateFromStore(duplicate) != FALSE;
			if (!removed)
			{
				CertFreeCertificateContext(duplicate);
			}
		}
		break;
	}
	if (certificate != nullptr)
	{
		CertFreeCertificateContext(certificate);
	}
	CertCloseStore(store, 0);
	return removed || certificate == nullptr;
}

bool IsUntrustedRoot(HRESULT status)
{
	return status == CERT_E_UNTRUSTEDROOT || status == CERT_E_CHAINING;
}

} // namespace

bool PrepareDriverPackage(const std::wstring& packageDir, HWND owner, PackageTrust& trust,
						  const Log& log, std::wstring& error)
{
	trust = {};
	error.clear();

	const std::wstring candidates[] = {packageDir + L"\\" + kSysName, packageDir + L"\\" + kCatName};
	std::wstring signedFile;
	HRESULT status = TRUST_E_NOSIGNATURE;
	for (const std::wstring& candidate : candidates)
	{
		if (!FileExists(candidate))
		{
			continue;
		}
		const HRESULT candidateStatus = VerifyTrust(candidate);
		if (candidateStatus == TRUST_E_NOSIGNATURE)
		{
			continue;
		}
		signedFile = candidate;
		status = candidateStatus;
		break;
	}
	if (signedFile.empty())
	{
		error = L"The driver package is not signed, or its signature could not be read. "
				L"Rebuild OpenA2dpSetup.exe from a signed driver package.";
		return false;
	}

	PCCERT_CONTEXT certificate = nullptr;
	if (!ExtractSigner(signedFile, certificate, error))
	{
		return false;
	}
	trust.signer = CertificateName(certificate);
	trust.thumbprint = CertificateThumbprint(certificate);
	const bool selfSigned = IsSelfSigned(certificate);

	if (trust.thumbprint.empty())
	{
		CertFreeCertificateContext(certificate);
		error = L"The driver signing certificate has no thumbprint.";
		return false;
	}
	if (selfSigned && !IsTestSigningEnabled())
	{
		CertFreeCertificateContext(certificate);
		error = L"This OpenA2DP build is test-signed, but Windows test signing is off on this PC.\n\n"
				L"Enable it from an elevated prompt with:\n"
				L"    bcdedit /set testsigning on\n\n"
				L"then restart Windows and run setup again. If Secure Boot is enabled, test signing "
				L"cannot be used; install a production-signed build instead.";
		return false;
	}

	log(L"Driver package signed by " + trust.signer + L".");
	if (status == S_OK)
	{
		CertFreeCertificateContext(certificate);
		return true;
	}
	if (!IsUntrustedRoot(status))
	{
		CertFreeCertificateContext(certificate);
		error = L"The driver package signature is not valid (" + HresultText(status) +
				L"). The package may be damaged or signed by a certificate Windows does not accept.";
		return false;
	}
	if (!selfSigned)
	{
		CertFreeCertificateContext(certificate);
		error = L"The driver package is signed, but its certificate chain is not trusted on this PC. "
				L"Install the issuing certificate authority, then run setup again.";
		return false;
	}

	const std::wstring prompt =
		L"The OpenA2DP driver is signed by:\n\n    " + trust.signer +
		L"\n\nThis certificate is not trusted on this PC. Windows will not load a test-signed "
		L"kernel driver until its signing certificate is trusted, so setup can add it to the "
		L"LocalMachine Root store.\n\n"
		L"Only continue if you built this package or trust where it came from.\n\n"
		L"Trust this certificate?";
	if (MessageBoxW(owner, prompt.c_str(), L"Trust the OpenA2DP driver certificate?",
					MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
	{
		CertFreeCertificateContext(certificate);
		error = L"The driver certificate was not trusted, so nothing was installed.";
		return false;
	}
	if (!AddToRoot(certificate, error))
	{
		CertFreeCertificateContext(certificate);
		return false;
	}
	const HRESULT after = VerifyTrust(signedFile);
	CertFreeCertificateContext(certificate);
	if (after != S_OK)
	{
		(void)RemoveFromRoot(trust.thumbprint);
		error = L"The driver certificate was added, but the package still does not verify (" +
				HresultText(after) + L"). The certificate was removed again.";
		return false;
	}
	trust.added = true;
	log(L"The signer certificate was added to the machine Root store.");
	return true;
}

bool RemoveTrustedCertificate(const std::wstring& thumbprint, std::wstring& error)
{
	error.clear();
	if (thumbprint.empty())
	{
		return true;
	}
	if (!RemoveFromRoot(thumbprint))
	{
		error = L"The OpenA2DP driver certificate could not be removed from the machine Root store.";
		return false;
	}
	return true;
}

} // namespace OpenA2dp::Setup
