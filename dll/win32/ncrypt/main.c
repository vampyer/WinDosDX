/*
 * PROJECT:     WinDosDX CNG key storage
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     ncrypt.dll without a key storage provider
 *
 * Programs import ncrypt.dll for persisted keys and certificate private
 * keys. WinDosDX has no key storage service yet: a provider can be opened
 * and enumerated (it holds no keys), and key operations report that they
 * are not supported, which programs handle as "no such key". The BCrypt
 * functions ncrypt re-exports are forwarded to bcrypt.dll (see the .spec).
 */

#include <windef.h>
#include <winbase.h>
#include <winerror.h>

typedef LONG SECURITY_STATUS;
typedef ULONG_PTR NCRYPT_HANDLE, NCRYPT_PROV_HANDLE, NCRYPT_KEY_HANDLE, NCRYPT_SECRET_HANDLE;

/* Provider handles are 1-based slots in a small table: nothing to free,
 * and any other value is recognised as invalid without touching memory. */
#define MAX_PROVIDERS 64
static LONG ProviderInUse[MAX_PROVIDERS];

static BOOL
IsProvider(NCRYPT_HANDLE Handle)
{
    return Handle >= 1 && Handle <= MAX_PROVIDERS && ProviderInUse[Handle - 1];
}

SECURITY_STATUS WINAPI
NCryptOpenStorageProvider(NCRYPT_PROV_HANDLE *phProvider, LPCWSTR pszProviderName, DWORD dwFlags)
{
    ULONG i;

    UNREFERENCED_PARAMETER(pszProviderName);
    UNREFERENCED_PARAMETER(dwFlags);

    if (!phProvider)
        return NTE_INVALID_PARAMETER;
    for (i = 0; i < MAX_PROVIDERS; i++)
    {
        if (InterlockedCompareExchange(&ProviderInUse[i], 1, 0) == 0)
        {
            *phProvider = i + 1;
            return ERROR_SUCCESS;
        }
    }
    return NTE_NO_MEMORY;
}

SECURITY_STATUS WINAPI
NCryptFreeObject(NCRYPT_HANDLE hObject)
{
    if (!IsProvider(hObject))
        return NTE_INVALID_HANDLE;
    InterlockedExchange(&ProviderInUse[hObject - 1], 0);
    return ERROR_SUCCESS;
}

SECURITY_STATUS WINAPI
NCryptFreeBuffer(PVOID pvInput)
{
    HeapFree(GetProcessHeap(), 0, pvInput);
    return ERROR_SUCCESS;
}

BOOL WINAPI
NCryptIsKeyHandle(NCRYPT_KEY_HANDLE hKey)
{
    /* No key handle is ever handed out. */
    UNREFERENCED_PARAMETER(hKey);
    return FALSE;
}

/* The one provider WinDosDX has, with no keys in it. */

SECURITY_STATUS WINAPI
NCryptEnumStorageProviders(DWORD *pdwProviderCount, PVOID *ppProviderList, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(dwFlags);

    if (!pdwProviderCount || !ppProviderList)
        return NTE_INVALID_PARAMETER;
    *pdwProviderCount = 0;
    *ppProviderList = NULL;
    return ERROR_SUCCESS;
}

SECURITY_STATUS WINAPI
NCryptEnumKeys(NCRYPT_PROV_HANDLE hProvider, LPCWSTR pszScope, PVOID *ppKeyName, PVOID *ppEnumState, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(pszScope);
    UNREFERENCED_PARAMETER(ppKeyName);
    UNREFERENCED_PARAMETER(ppEnumState);
    UNREFERENCED_PARAMETER(dwFlags);
    return IsProvider(hProvider) ? NTE_NO_MORE_ITEMS : NTE_INVALID_HANDLE;
}

SECURITY_STATUS WINAPI
NCryptEnumAlgorithms(NCRYPT_PROV_HANDLE hProvider, DWORD dwAlgOperations, DWORD *pdwAlgCount,
                     PVOID *ppAlgList, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(dwAlgOperations);
    UNREFERENCED_PARAMETER(dwFlags);

    if (!IsProvider(hProvider))
        return NTE_INVALID_HANDLE;
    if (!pdwAlgCount || !ppAlgList)
        return NTE_INVALID_PARAMETER;
    *pdwAlgCount = 0;
    *ppAlgList = NULL;
    return ERROR_SUCCESS;
}

SECURITY_STATUS WINAPI
NCryptIsAlgSupported(NCRYPT_PROV_HANDLE hProvider, LPCWSTR pszAlgId, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(pszAlgId);
    UNREFERENCED_PARAMETER(dwFlags);
    return IsProvider(hProvider) ? NTE_NOT_SUPPORTED : NTE_INVALID_HANDLE;
}

SECURITY_STATUS WINAPI
NCryptOpenKey(NCRYPT_PROV_HANDLE hProvider, NCRYPT_KEY_HANDLE *phKey, LPCWSTR pszKeyName,
              DWORD dwLegacyKeySpec, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(pszKeyName);
    UNREFERENCED_PARAMETER(dwLegacyKeySpec);
    UNREFERENCED_PARAMETER(dwFlags);

    if (!IsProvider(hProvider))
        return NTE_INVALID_HANDLE;
    if (phKey)
        *phKey = 0;
    return NTE_BAD_KEYSET;
}

SECURITY_STATUS WINAPI
NCryptCreatePersistedKey(NCRYPT_PROV_HANDLE hProvider, NCRYPT_KEY_HANDLE *phKey, LPCWSTR pszAlgId,
                         LPCWSTR pszKeyName, DWORD dwLegacyKeySpec, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(pszAlgId);
    UNREFERENCED_PARAMETER(pszKeyName);
    UNREFERENCED_PARAMETER(dwLegacyKeySpec);
    UNREFERENCED_PARAMETER(dwFlags);

    if (!IsProvider(hProvider))
        return NTE_INVALID_HANDLE;
    if (phKey)
        *phKey = 0;
    return NTE_NOT_SUPPORTED;
}

SECURITY_STATUS WINAPI
NCryptImportKey(NCRYPT_PROV_HANDLE hProvider, NCRYPT_KEY_HANDLE hImportKey, LPCWSTR pszBlobType,
                PVOID pParameterList, NCRYPT_KEY_HANDLE *phKey, PBYTE pbData, DWORD cbData, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(hImportKey);
    UNREFERENCED_PARAMETER(pszBlobType);
    UNREFERENCED_PARAMETER(pParameterList);
    UNREFERENCED_PARAMETER(pbData);
    UNREFERENCED_PARAMETER(cbData);
    UNREFERENCED_PARAMETER(dwFlags);

    if (!IsProvider(hProvider))
        return NTE_INVALID_HANDLE;
    if (phKey)
        *phKey = 0;
    return NTE_NOT_SUPPORTED;
}

SECURITY_STATUS WINAPI
NCryptTranslateHandle(NCRYPT_PROV_HANDLE *phProvider, NCRYPT_KEY_HANDLE *phKey, ULONG_PTR hLegacyProv,
                      ULONG_PTR hLegacyKey, DWORD dwLegacyKeySpec, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(phProvider);
    UNREFERENCED_PARAMETER(phKey);
    UNREFERENCED_PARAMETER(hLegacyProv);
    UNREFERENCED_PARAMETER(hLegacyKey);
    UNREFERENCED_PARAMETER(dwLegacyKeySpec);
    UNREFERENCED_PARAMETER(dwFlags);
    return NTE_NOT_SUPPORTED;
}

/* Properties: a provider has none that can be read or set. */

SECURITY_STATUS WINAPI
NCryptGetProperty(NCRYPT_HANDLE hObject, LPCWSTR pszProperty, PBYTE pbOutput, DWORD cbOutput,
                  DWORD *pcbResult, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(pszProperty);
    UNREFERENCED_PARAMETER(pbOutput);
    UNREFERENCED_PARAMETER(cbOutput);
    UNREFERENCED_PARAMETER(pcbResult);
    UNREFERENCED_PARAMETER(dwFlags);
    return IsProvider(hObject) ? NTE_NOT_SUPPORTED : NTE_INVALID_HANDLE;
}

SECURITY_STATUS WINAPI
NCryptSetProperty(NCRYPT_HANDLE hObject, LPCWSTR pszProperty, PBYTE pbInput, DWORD cbInput, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(pszProperty);
    UNREFERENCED_PARAMETER(pbInput);
    UNREFERENCED_PARAMETER(cbInput);
    UNREFERENCED_PARAMETER(dwFlags);
    return IsProvider(hObject) ? NTE_NOT_SUPPORTED : NTE_INVALID_HANDLE;
}

/* Key operations: there are no keys, so every handle is invalid. */

SECURITY_STATUS WINAPI
NCryptFinalizeKey(NCRYPT_KEY_HANDLE hKey, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(hKey);
    UNREFERENCED_PARAMETER(dwFlags);
    return NTE_INVALID_HANDLE;
}

SECURITY_STATUS WINAPI
NCryptDeleteKey(NCRYPT_KEY_HANDLE hKey, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(hKey);
    UNREFERENCED_PARAMETER(dwFlags);
    return NTE_INVALID_HANDLE;
}

SECURITY_STATUS WINAPI
NCryptExportKey(NCRYPT_KEY_HANDLE hKey, NCRYPT_KEY_HANDLE hExportKey, LPCWSTR pszBlobType,
                PVOID pParameterList, PBYTE pbOutput, DWORD cbOutput, DWORD *pcbResult, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(hKey);
    UNREFERENCED_PARAMETER(hExportKey);
    UNREFERENCED_PARAMETER(pszBlobType);
    UNREFERENCED_PARAMETER(pParameterList);
    UNREFERENCED_PARAMETER(pbOutput);
    UNREFERENCED_PARAMETER(cbOutput);
    UNREFERENCED_PARAMETER(pcbResult);
    UNREFERENCED_PARAMETER(dwFlags);
    return NTE_INVALID_HANDLE;
}

SECURITY_STATUS WINAPI
NCryptSignHash(NCRYPT_KEY_HANDLE hKey, PVOID pPaddingInfo, PBYTE pbHashValue, DWORD cbHashValue,
               PBYTE pbSignature, DWORD cbSignature, DWORD *pcbResult, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(hKey);
    UNREFERENCED_PARAMETER(pPaddingInfo);
    UNREFERENCED_PARAMETER(pbHashValue);
    UNREFERENCED_PARAMETER(cbHashValue);
    UNREFERENCED_PARAMETER(pbSignature);
    UNREFERENCED_PARAMETER(cbSignature);
    UNREFERENCED_PARAMETER(pcbResult);
    UNREFERENCED_PARAMETER(dwFlags);
    return NTE_INVALID_HANDLE;
}

SECURITY_STATUS WINAPI
NCryptVerifySignature(NCRYPT_KEY_HANDLE hKey, PVOID pPaddingInfo, PBYTE pbHashValue, DWORD cbHashValue,
                      PBYTE pbSignature, DWORD cbSignature, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(hKey);
    UNREFERENCED_PARAMETER(pPaddingInfo);
    UNREFERENCED_PARAMETER(pbHashValue);
    UNREFERENCED_PARAMETER(cbHashValue);
    UNREFERENCED_PARAMETER(pbSignature);
    UNREFERENCED_PARAMETER(cbSignature);
    UNREFERENCED_PARAMETER(dwFlags);
    return NTE_INVALID_HANDLE;
}

SECURITY_STATUS WINAPI
NCryptEncrypt(NCRYPT_KEY_HANDLE hKey, PBYTE pbInput, DWORD cbInput, PVOID pPaddingInfo,
              PBYTE pbOutput, DWORD cbOutput, DWORD *pcbResult, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(hKey);
    UNREFERENCED_PARAMETER(pbInput);
    UNREFERENCED_PARAMETER(cbInput);
    UNREFERENCED_PARAMETER(pPaddingInfo);
    UNREFERENCED_PARAMETER(pbOutput);
    UNREFERENCED_PARAMETER(cbOutput);
    UNREFERENCED_PARAMETER(pcbResult);
    UNREFERENCED_PARAMETER(dwFlags);
    return NTE_INVALID_HANDLE;
}

SECURITY_STATUS WINAPI
NCryptDecrypt(NCRYPT_KEY_HANDLE hKey, PBYTE pbInput, DWORD cbInput, PVOID pPaddingInfo,
              PBYTE pbOutput, DWORD cbOutput, DWORD *pcbResult, DWORD dwFlags)
{
    return NCryptEncrypt(hKey, pbInput, cbInput, pPaddingInfo, pbOutput, cbOutput, pcbResult, dwFlags);
}

SECURITY_STATUS WINAPI
NCryptSecretAgreement(NCRYPT_KEY_HANDLE hPrivKey, NCRYPT_KEY_HANDLE hPubKey,
                      NCRYPT_SECRET_HANDLE *phAgreedSecret, DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(hPrivKey);
    UNREFERENCED_PARAMETER(hPubKey);
    UNREFERENCED_PARAMETER(phAgreedSecret);
    UNREFERENCED_PARAMETER(dwFlags);
    return NTE_INVALID_HANDLE;
}

SECURITY_STATUS WINAPI
NCryptDeriveKey(NCRYPT_SECRET_HANDLE hSharedSecret, LPCWSTR pwszKDF, PVOID pParameterList,
                PBYTE pbDerivedKey, DWORD cbDerivedKey, DWORD *pcbResult, ULONG dwFlags)
{
    UNREFERENCED_PARAMETER(hSharedSecret);
    UNREFERENCED_PARAMETER(pwszKDF);
    UNREFERENCED_PARAMETER(pParameterList);
    UNREFERENCED_PARAMETER(pbDerivedKey);
    UNREFERENCED_PARAMETER(cbDerivedKey);
    UNREFERENCED_PARAMETER(pcbResult);
    UNREFERENCED_PARAMETER(dwFlags);
    return NTE_INVALID_HANDLE;
}
