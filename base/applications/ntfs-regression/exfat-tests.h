/*
 * Helpers shared by the exFAT read tests (exfat-tests.c) and write tests
 * (exfat-write-tests.c).
 */

#pragma once

#define CHUNK_SIZE (64 * 1024)

extern WCHAR ExfatRoot[4];
/* TRUE on the Windows-made test image (label EXFATREG), whose files the
   read suite and the append test expect. Any other exFAT volume only runs
   the tests that create their own files. */
extern BOOL ExfatIsTemplate;

VOID Emit(const char *Format, ...);
VOID ExfatPath(PWSTR Buffer, PCWSTR Relative);
BYTE PatternByte(ULONGLONG Offset, BYTE Seed);
BOOL CheckTextFile(PCWSTR Relative, const char *Expected, const char *Name);
BOOL CountMatches(PCWSTR Relative, PDWORD Files, PDWORD Dots);
