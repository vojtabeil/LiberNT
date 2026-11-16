/*
 * PROJECT:     LiberNT certutil
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     CertUtil decodehex and encodehex implementation
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "precomp.h"
#include <wincrypt.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * The text forms are those of Windows certutil and CryptBinaryToString:
 * 16 bytes a line, "xx" separated by a space and by two in the middle,
 * an address "%04x" and a tab before the bytes (ADDR), and the bytes as
 * characters after three spaces, the hex part padded to a full line (ASCII).
 */
#define HEX_LINE_BYTES 16
#define HEX_LINE_WIDTH (HEX_LINE_BYTES * 3)

static void PrintFailure(LPCWSTR VerbName, DWORD Error)
{
    WCHAR Message[256];

    ConPrintf(StdOut, L"CertUtil: -%s command FAILED: 0x%08lx (WIN32: %lu)\n",
              VerbName, (ULONG)HRESULT_FROM_WIN32(Error), Error);
    if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, Error, 0,
                       Message, RTL_NUMBER_OF(Message), NULL))
    {
        ConPrintf(StdOut, L"CertUtil: %s", Message);
    }
}

static BOOL ParseType(LPCWSTR VerbName, LPCWSTR Text, DWORD Default, DWORD *Type)
{
    WCHAR *End;

    if (!Text)
    {
        *Type = Default;
        return TRUE;
    }

    *Type = wcstoul(Text, &End, 0);
    if (*End || End == Text)
    {
        ConPrintf(StdOut, L"CertUtil: -%s: invalid type: %s\n", VerbName, Text);
        return FALSE;
    }
    return TRUE;
}

static BOOL ReadWholeFile(LPCWSTR Name, char **Data, DWORD *Length)
{
    LARGE_INTEGER Size;
    HANDLE hIn;
    BOOL bSuccess = FALSE;

    *Data = NULL;
    *Length = 0;

    hIn = CreateFileW(Name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hIn == INVALID_HANDLE_VALUE)
        return FALSE;

    if (!GetFileSizeEx(hIn, &Size) || Size.QuadPart >= MAXDWORD)
    {
        CloseHandle(hIn);
        SetLastError(ERROR_FILE_TOO_LARGE);
        return FALSE;
    }

    *Data = (char *)malloc(Size.LowPart + 1);
    if (*Data)
    {
        bSuccess = ReadFile(hIn, *Data, Size.LowPart, Length, NULL);
        (*Data)[*Length] = 0;
    }
    else
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    }

    CloseHandle(hIn);
    return bSuccess;
}

static BOOL WriteWholeFile(LPCWSTR Name, const void *Data, DWORD Length)
{
    HANDLE hOut;
    DWORD Written;
    BOOL bSuccess;

    /* Like Windows certutil, an existing file is replaced only with -f */
    hOut = CreateFileW(Name, GENERIC_WRITE, 0, NULL, g_Force ? CREATE_ALWAYS : CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut == INVALID_HANDLE_VALUE)
        return FALSE;

    bSuccess = WriteFile(hOut, Data, Length, &Written, NULL) && Written == Length;
    CloseHandle(hOut);
    return bSuccess;
}

static BOOL IsHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static BOOL IsBlank(char c)
{
    return c == ' ' || c == '\t';
}

/*
 * Copies the hex bytes of the lines of an ADDR, ASCII or ASCIIADDR dump to
 * Out as plain hex, one byte a time, dropping the addresses and characters.
 */
static BOOL ExtractDumpBytes(const char *Text, DWORD Length, BOOL Addr, BOOL Ascii, char *Out, DWORD *OutLength)
{
    const char *p = Text, *End = Text + Length;
    DWORD n = 0;

    while (p < End)
    {
        const char *Eol = p;
        int Bytes = 0;

        while (Eol < End && *Eol != '\n')
            Eol++;

        /* Lines may end in CR LF */
        const char *LineEnd = (Eol > p && Eol[-1] == '\r') ? Eol - 1 : Eol;

        while (p < LineEnd && IsBlank(*p))
            p++;

        if (p < LineEnd && Addr)
        {
            const char *Start = p;

            while (p < LineEnd && IsHexDigit(*p))
                p++;
            /* An address of 4 to 8 digits, then blanks and the bytes */
            if (p - Start < 4 || p - Start > 8 || p == LineEnd || !IsBlank(*p))
                return FALSE;
        }

        while (p < LineEnd)
        {
            int Blanks = 0;

            while (p < LineEnd && IsBlank(*p))
            {
                p++;
                Blanks++;
            }
            if (p == LineEnd)
                break;

            /* Three blanks after the bytes start the characters */
            if (Bytes && Blanks >= 3)
            {
                if (!Ascii)
                    return FALSE;
                break;
            }

            if (p + 1 >= LineEnd || !IsHexDigit(p[0]) || !IsHexDigit(p[1]) ||
                (p + 2 < LineEnd && !IsBlank(p[2])) || Bytes == HEX_LINE_BYTES)
            {
                return FALSE;
            }

            Out[n++] = p[0];
            Out[n++] = p[1];
            Bytes++;
            p += 2;
        }

        p = Eol < End ? Eol + 1 : End;
    }

    *OutLength = n;
    return TRUE;
}

static BOOL DecodeAs(const char *Text, DWORD Length, DWORD Type, BYTE **Data, DWORD *DataLength)
{
    const char *Hex = Text;
    DWORD HexLength = Length;
    char *Plain = NULL;
    BOOL bSuccess;

    *Data = NULL;
    *DataLength = 0;

    if (Type == CRYPT_STRING_HEXADDR || Type == CRYPT_STRING_HEXASCII || Type == CRYPT_STRING_HEXASCIIADDR)
    {
        Plain = (char *)malloc(Length + 1);
        if (!Plain)
            return FALSE;
        if (!ExtractDumpBytes(Text, Length, Type != CRYPT_STRING_HEXASCII, Type != CRYPT_STRING_HEXADDR,
                              Plain, &HexLength))
        {
            free(Plain);
            SetLastError(ERROR_INVALID_DATA);
            return FALSE;
        }
        Hex = Plain;
        Type = CRYPT_STRING_HEX;
    }

    if (HexLength == 0)
    {
        free(Plain);
        *Data = (BYTE *)malloc(1);
        return *Data != NULL;
    }

    bSuccess = CryptStringToBinaryA(Hex, HexLength, Type, NULL, DataLength, NULL, NULL);
    if (bSuccess)
    {
        *Data = (BYTE *)malloc(*DataLength ? *DataLength : 1);
        bSuccess = *Data && CryptStringToBinaryA(Hex, HexLength, Type, *Data, DataLength, NULL, NULL);
        if (!bSuccess)
        {
            free(*Data);
            *Data = NULL;
        }
    }

    free(Plain);
    return bSuccess;
}

BOOL decode_hex(LPCWSTR InFile, LPCWSTR OutFile, LPCWSTR TypeText)
{
    /* As CRYPT_STRING_HEX_ANY: a dump with addresses first, plain hex next, a dump with characters last */
    static const DWORD AnyOrder[] = { CRYPT_STRING_HEXADDR, CRYPT_STRING_HEXASCIIADDR,
                                      CRYPT_STRING_HEX, CRYPT_STRING_HEXASCII };
    char *Text = NULL;
    BYTE *Data = NULL;
    DWORD Read = 0, Length = 0, Type;
    BOOL bSuccess = FALSE;

    if (!ParseType(L"decodehex", TypeText, CRYPT_STRING_HEX_ANY, &Type))
        return FALSE;

    if (ReadWholeFile(InFile, &Text, &Read))
    {
        if (Type == CRYPT_STRING_HEX_ANY)
        {
            for (size_t n = 0; n < RTL_NUMBER_OF(AnyOrder) && !bSuccess; ++n)
                bSuccess = DecodeAs(Text, Read, AnyOrder[n], &Data, &Length);
        }
        else
        {
            bSuccess = DecodeAs(Text, Read, Type, &Data, &Length);
        }

        if (bSuccess)
        {
            ConPrintf(StdOut, L"Input Length = %lu\n", Read);
            bSuccess = WriteWholeFile(OutFile, Data, Length);
            if (bSuccess)
                ConPrintf(StdOut, L"Output Length = %lu\n", Length);
        }
    }

    if (!bSuccess)
        PrintFailure(L"decodehex", GetLastError());

    free(Data);
    free(Text);
    return bSuccess;
}

static DWORD FormatDump(const BYTE *Data, DWORD Length, BOOL Addr, BOOL Ascii, char *Out)
{
    static const char Digits[] = "0123456789abcdef";
    DWORD Offset, n = 0;

    for (Offset = 0; Offset < Length; Offset += HEX_LINE_BYTES)
    {
        DWORD Count = min(HEX_LINE_BYTES, Length - Offset), i, Width = 0;

        if (Addr)
            n += sprintf(Out + n, "%04lx\t", Offset);

        for (i = 0; i < Count; i++)
        {
            if (i)
            {
                Out[n++] = ' ';
                Width++;
                if (i == HEX_LINE_BYTES / 2)
                {
                    Out[n++] = ' ';
                    Width++;
                }
            }
            Out[n++] = Digits[Data[Offset + i] >> 4];
            Out[n++] = Digits[Data[Offset + i] & 0xf];
            Width += 2;
        }

        if (Ascii)
        {
            while (Width < HEX_LINE_WIDTH)
            {
                Out[n++] = ' ';
                Width++;
            }
            Out[n++] = ' ';
            Out[n++] = ' ';
            Out[n++] = ' ';
            for (i = 0; i < Count; i++)
            {
                BYTE b = Data[Offset + i];
                Out[n++] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
            }
        }

        Out[n++] = '\r';
        Out[n++] = '\n';
    }

    return n;
}

BOOL encode_hex(LPCWSTR InFile, LPCWSTR OutFile, LPCWSTR TypeText)
{
    char *Data = NULL;
    char *Text = NULL;
    DWORD Read = 0, Length = 0, Type;
    BOOL bSuccess = FALSE;

    if (!ParseType(L"encodehex", TypeText, CRYPT_STRING_HEXASCIIADDR, &Type))
        return FALSE;

    if (ReadWholeFile(InFile, &Data, &Read))
    {
        ConPrintf(StdOut, L"Input Length = %lu\n", Read);

        if (Type == CRYPT_STRING_HEX || Type == CRYPT_STRING_HEXASCII ||
            Type == CRYPT_STRING_HEXADDR || Type == CRYPT_STRING_HEXASCIIADDR)
        {
            DWORD Lines = (Read + HEX_LINE_BYTES - 1) / HEX_LINE_BYTES;

            /* address and tab, bytes, padding, three blanks, characters, CR LF, terminator */
            Text = (char *)malloc(Lines * (16 + HEX_LINE_WIDTH + 3 + HEX_LINE_BYTES + 2) + 1);
            if (Text)
            {
                Length = FormatDump((const BYTE *)Data, Read,
                                    Type == CRYPT_STRING_HEXADDR || Type == CRYPT_STRING_HEXASCIIADDR,
                                    Type == CRYPT_STRING_HEXASCII || Type == CRYPT_STRING_HEXASCIIADDR,
                                    Text);
                bSuccess = TRUE;
            }
            else
            {
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            }
        }
        else if (CryptBinaryToStringA((const BYTE *)Data, Read, Type, NULL, &Length))
        {
            Text = (char *)malloc(Length ? Length : 1);
            bSuccess = Text && CryptBinaryToStringA((const BYTE *)Data, Read, Type, Text, &Length);
        }

        if (bSuccess)
        {
            bSuccess = WriteWholeFile(OutFile, Text, Length);
            if (bSuccess)
                ConPrintf(StdOut, L"Output Length = %lu\n", Length);
        }
    }

    if (!bSuccess)
        PrintFailure(L"encodehex", GetLastError());

    free(Text);
    free(Data);
    return bSuccess;
}
