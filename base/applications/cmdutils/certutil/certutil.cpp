/*
 * PROJECT:     ReactOS certutil
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     CertUtil commandline handling
 * COPYRIGHT:   Copyright 2020 Mark Jansen (mark.jansen@reactos.org)
 *
 * Note: Only -hashfile, -asn, -decodehex and -encodehex are implemented for now, the rest is not present!
 */

#include "precomp.h"
#include <wincrypt.h>
#include <stdlib.h>

/* -f: a verb that writes a file replaces one that exists */
BOOL g_Force;

typedef struct
{
    LPCWSTR Name;
    int MinArgs;
    int MaxArgs;
    BOOL (*pfn)(int Count, LPCWSTR *Args);
} Verb;

static BOOL hash_file_verb(int Count, LPCWSTR *Args)
{
    return hash_file(Args[0], Count > 1 ? Args[1] : NULL);
}

static BOOL asn_dump_verb(int Count, LPCWSTR *Args)
{
    return asn_dump(Args[0], Count > 1 ? Args[1] : NULL);
}

static BOOL decode_hex_verb(int Count, LPCWSTR *Args)
{
    return decode_hex(Args[0], Args[1], Count > 2 ? Args[2] : NULL);
}

static BOOL encode_hex_verb(int Count, LPCWSTR *Args)
{
    return encode_hex(Args[0], Args[1], Count > 2 ? Args[2] : NULL);
}

Verb verbs[] = {
    { L"hashfile", 1, 2, hash_file_verb },
    { L"asn", 1, 2, asn_dump_verb },
    { L"decodehex", 2, 3, decode_hex_verb },
    { L"encodehex", 2, 3, encode_hex_verb },
};

static void print_usage()
{
    ConPuts(StdOut, L"Verbs:\n");
    ConPuts(StdOut, L"  -hashfile           -- Display cryptographic hash over a file\n");
    ConPuts(StdOut, L"                         Usage: -hashfile <file> [MD2|MD4|MD5|SHA1|SHA256|SHA384|SHA512]\n");
    ConPuts(StdOut, L"  -asn                -- Display ASN.1 encoding of a file\n");
    ConPuts(StdOut, L"  -decodehex          -- Decode a hexadecimal-encoded file\n");
    ConPuts(StdOut, L"                         Usage: [-f] -decodehex <infile> <outfile> [type]\n");
    ConPuts(StdOut, L"  -encodehex          -- Encode a file in hexadecimal\n");
    ConPuts(StdOut, L"                         Usage: [-f] -encodehex <infile> <outfile> [type]\n");
    ConPuts(StdOut, L"                         type -- numeric CRYPT_STRING_* encoding type\n");
    ConPuts(StdOut, L"\n");
    ConPuts(StdOut, L"Options:\n");
    ConPuts(StdOut, L"  -f                  -- Force overwrite\n");
    ConPuts(StdOut, L"\n");
    ConPuts(StdOut, L"CertUtil -?           -- Display a list of all verbs\n");
    ConPuts(StdOut, L"CertUtil -hashfile -? -- Display help text for the 'hashfile' verb\n");
}


Verb* MatchVerb(LPCWSTR arg)
{
    if (arg[0] != '-' && arg[0] != '/')
        return NULL;

    for (size_t n = 0; n < RTL_NUMBER_OF(verbs); ++n)
    {
        if (!_wcsicmp(verbs[n].Name, arg + 1))
        {
            return verbs + n;
        }
    }

    return NULL;
}

static BOOL IsOption(LPCWSTR arg, LPCWSTR Name)
{
    return (arg[0] == '-' || arg[0] == '/') && !_wcsicmp(arg + 1, Name);
}

EXTERN_C int wmain(int argc, WCHAR *argv[])
{
    LPCWSTR Args[8];
    Verb* verb = NULL;
    int Count = 0;
    int n;

    /* Initialize the Console Standard Streams */
    ConInitStdStreams();

    if (argc == 1) /* i.e. no commandline arguments given */
    {
        print_usage();
        return EXIT_SUCCESS;
    }

    /* Options may stand before or after the verb, as with Windows certutil */
    for (n = 1; n < argc; ++n)
    {
        if (!_wcsicmp(argv[n], L"-?") || !_wcsicmp(argv[n], L"/?"))
        {
            print_usage();
            return EXIT_SUCCESS;
        }

        if (IsOption(argv[n], L"f"))
        {
            g_Force = TRUE;
            continue;
        }

        if (!verb)
        {
            verb = MatchVerb(argv[n]);
            if (!verb)
            {
                ConPrintf(StdOut, L"CertUtil: Unknown verb: %s\n", argv[n]);
                return EXIT_FAILURE;
            }
            continue;
        }

        if (Count == RTL_NUMBER_OF(Args))
        {
            Count++;
            break;
        }
        Args[Count++] = argv[n];
    }

    if (!verb)
    {
        print_usage();
        return EXIT_SUCCESS;
    }

    if (Count < verb->MinArgs || Count > verb->MaxArgs)
    {
        ConPrintf(StdOut, L"CertUtil: -%s expected %d to %d arguments, got %d\n",
                  verb->Name, verb->MinArgs, verb->MaxArgs, Count);
        return EXIT_FAILURE;
    }

    if (!verb->pfn(Count, Args))
    {
        /* The verb prints the failure */
        return EXIT_FAILURE;
    }

    ConPrintf(StdOut, L"CertUtil: -%s command completed successfully\n", verb->Name);
    return EXIT_SUCCESS;
}
