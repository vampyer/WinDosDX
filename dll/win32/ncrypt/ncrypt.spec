# ncrypt.dll: CNG key storage without a provider; BCrypt goes to bcrypt.dll.

@ stdcall BCryptAddContextFunction(long wstr long wstr long) bcrypt.BCryptAddContextFunction
@ stdcall BCryptAddContextFunctionProvider(long wstr long wstr wstr long) bcrypt.BCryptAddContextFunctionProvider
@ stdcall BCryptCloseAlgorithmProvider(ptr long) bcrypt.BCryptCloseAlgorithmProvider
@ stdcall BCryptCreateHash(ptr ptr ptr long ptr long long) bcrypt.BCryptCreateHash
@ stdcall BCryptDecrypt(ptr ptr long ptr ptr long ptr long ptr long) bcrypt.BCryptDecrypt
@ stdcall BCryptDeriveKey(ptr wstr ptr ptr long ptr long) bcrypt.BCryptDeriveKey
@ stdcall BCryptDeriveKeyCapi(ptr ptr ptr long long) bcrypt.BCryptDeriveKeyCapi
@ stdcall BCryptDeriveKeyPBKDF2(ptr ptr long ptr long int64 ptr long long) bcrypt.BCryptDeriveKeyPBKDF2
@ stdcall BCryptDestroyHash(ptr) bcrypt.BCryptDestroyHash
@ stdcall BCryptDestroyKey(ptr) bcrypt.BCryptDestroyKey
@ stdcall BCryptDestroySecret(ptr) bcrypt.BCryptDestroySecret
@ stdcall BCryptDuplicateHash(ptr ptr ptr long long) bcrypt.BCryptDuplicateHash
@ stdcall BCryptDuplicateKey(ptr ptr ptr long long) bcrypt.BCryptDuplicateKey
@ stdcall BCryptEncrypt(ptr ptr long ptr ptr long ptr long ptr long) bcrypt.BCryptEncrypt
@ stdcall BCryptEnumAlgorithms(long ptr ptr long) bcrypt.BCryptEnumAlgorithms
@ stdcall BCryptEnumContextFunctions(long wstr long ptr ptr) bcrypt.BCryptEnumContextFunctions
@ stdcall BCryptExportKey(ptr ptr wstr ptr long ptr long) bcrypt.BCryptExportKey
@ stdcall BCryptFinalizeKeyPair(ptr long) bcrypt.BCryptFinalizeKeyPair
@ stdcall BCryptFinishHash(ptr ptr long long) bcrypt.BCryptFinishHash
@ stdcall BCryptFreeBuffer(ptr) bcrypt.BCryptFreeBuffer
@ stdcall BCryptGenerateKeyPair(ptr ptr long long) bcrypt.BCryptGenerateKeyPair
@ stdcall BCryptGenerateSymmetricKey(ptr ptr ptr long ptr long long) bcrypt.BCryptGenerateSymmetricKey
@ stdcall BCryptGenRandom(ptr ptr long long) bcrypt.BCryptGenRandom
@ stdcall BCryptGetFipsAlgorithmMode(ptr) bcrypt.BCryptGetFipsAlgorithmMode
@ stdcall BCryptGetProperty(ptr wstr ptr long ptr long) bcrypt.BCryptGetProperty
@ stdcall BCryptHash(ptr ptr long ptr long ptr long) bcrypt.BCryptHash
@ stdcall BCryptHashData(ptr ptr long long) bcrypt.BCryptHashData
@ stdcall BCryptImportKey(ptr ptr wstr ptr ptr long ptr long long) bcrypt.BCryptImportKey
@ stdcall BCryptImportKeyPair(ptr ptr wstr ptr ptr long long) bcrypt.BCryptImportKeyPair
@ stdcall BCryptKeyDerivation(ptr ptr ptr long ptr long) bcrypt.BCryptKeyDerivation
@ stdcall BCryptOpenAlgorithmProvider(ptr wstr wstr long) bcrypt.BCryptOpenAlgorithmProvider
@ stdcall BCryptRegisterProvider(wstr long ptr) bcrypt.BCryptRegisterProvider
@ stdcall BCryptRemoveContextFunction(long wstr long wstr) bcrypt.BCryptRemoveContextFunction
@ stdcall BCryptRemoveContextFunctionProvider(long wstr long wstr wstr) bcrypt.BCryptRemoveContextFunctionProvider
@ stdcall BCryptSecretAgreement(ptr ptr ptr long) bcrypt.BCryptSecretAgreement
@ stdcall BCryptSetProperty(ptr wstr ptr long long) bcrypt.BCryptSetProperty
@ stdcall BCryptSignHash(ptr ptr ptr long ptr long ptr long) bcrypt.BCryptSignHash
@ stdcall BCryptUnregisterProvider(wstr) bcrypt.BCryptUnregisterProvider
@ stdcall BCryptVerifySignature(ptr ptr ptr long ptr long long) bcrypt.BCryptVerifySignature
@ stdcall NCryptCreatePersistedKey(ptr ptr wstr wstr long long)
@ stdcall NCryptDecrypt(ptr ptr long ptr ptr long ptr long)
@ stdcall NCryptDeleteKey(ptr long)
@ stdcall NCryptDeriveKey(ptr wstr ptr ptr long ptr long)
@ stdcall NCryptEncrypt(ptr ptr long ptr ptr long ptr long)
@ stdcall NCryptEnumAlgorithms(ptr long ptr ptr long)
@ stdcall NCryptEnumKeys(ptr wstr ptr ptr long)
@ stdcall NCryptEnumStorageProviders(ptr ptr long)
@ stdcall NCryptExportKey(ptr ptr wstr ptr ptr long ptr long)
@ stdcall NCryptFinalizeKey(ptr long)
@ stdcall NCryptFreeBuffer(ptr)
@ stdcall NCryptFreeObject(ptr)
@ stdcall NCryptGetProperty(ptr wstr ptr long ptr long)
@ stdcall NCryptImportKey(ptr ptr wstr ptr ptr ptr long long)
@ stdcall NCryptIsAlgSupported(ptr wstr long)
@ stdcall NCryptIsKeyHandle(ptr)
@ stdcall NCryptOpenKey(ptr ptr wstr long long)
@ stdcall NCryptOpenStorageProvider(ptr wstr long)
@ stdcall NCryptSecretAgreement(ptr ptr ptr long)
@ stdcall NCryptSetProperty(ptr wstr ptr long long)
@ stdcall NCryptSignHash(ptr ptr ptr long ptr long ptr long)
@ stdcall NCryptTranslateHandle(ptr ptr ptr ptr long long)
@ stdcall NCryptVerifySignature(ptr ptr ptr long ptr long long)
