#include <windows.h>
#include <ntsecapi.h>

static int matches(LSA_UNICODE_STRING name, const WCHAR *text, USHORT count)
{
    if (!name.Buffer || name.Length != count * sizeof(WCHAR)) return 0;
    for (USHORT i = 0; i < count; ++i) if (name.Buffer[i] != text[i]) return 0;
    return 1;
}

int raven_entry(void)
{
    SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY;
    SID_IDENTIFIER_AUTHORITY unknown = {{0, 0, 0, 0, 0, 99}};
    PSID sids[2] = {0};
    PLSA_REFERENCED_DOMAIN_LIST domains = 0;
    PLSA_TRANSLATED_NAME names = 0;
    LSA_OBJECT_ATTRIBUTES attributes = {0};
    LSA_HANDLE policy = 0;
    attributes.Length = sizeof attributes;
    if (LsaOpenPolicy(0, &attributes, POLICY_LOOKUP_NAMES, &policy) ||
        !AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &sids[0]) ||
        !AllocateAndInitializeSid(&unknown, 1, 123, 0, 0, 0, 0, 0, 0, 0, &sids[1])) return 1;
    NTSTATUS status = LsaLookupSids(policy, 2, sids, &domains, &names);
    if (status != 0x107 || !domains || !names || names[0].Use != SidTypeWellKnownGroup ||
        names[0].Name.Length != 16 || names[0].Name.Buffer[0] != L'E' || names[1].Use != SidTypeUnknown ||
        names[1].DomainIndex != -1) return 2;
    LsaFreeMemory(domains);
    LsaFreeMemory(names);
    domains = 0; names = 0;
    if (LsaLookupSids(policy, 1, sids, &domains, &names) || !names || names[0].Name.Length != 16) return 3;
    LsaFreeMemory(domains);
    LsaFreeMemory(names);
    FreeSid(sids[0]); FreeSid(sids[1]);
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID admin = 0;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &admin)) return 4;
    domains = 0; names = 0;
    if (LsaLookupSids(policy, 1, &admin, &domains, &names) || !domains || domains->Entries != 1 ||
        !domains->Domains || domains->Domains[0].Name.Length != 14 ||
        !matches(domains->Domains[0].Name, L"BUILTIN", 7) || !IsValidSid(domains->Domains[0].Sid) ||
        *GetSidSubAuthorityCount(domains->Domains[0].Sid) != 1 ||
        *GetSidSubAuthority(domains->Domains[0].Sid, 0) != SECURITY_BUILTIN_DOMAIN_RID ||
        !names || names[0].DomainIndex != 0 || names[0].Use != SidTypeAlias ||
        names[0].Name.Length != 28 || !matches(names[0].Name, L"Administrators", 14)) return 5;
    LsaFreeMemory(domains); LsaFreeMemory(names); FreeSid(admin);
    LsaClose(policy);
    return 0;
}
