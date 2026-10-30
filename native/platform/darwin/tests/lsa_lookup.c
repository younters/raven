#include <windows.h>
#include <ntsecapi.h>

void raven_entry(void)
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
        !AllocateAndInitializeSid(&unknown, 1, 123, 0, 0, 0, 0, 0, 0, 0, &sids[1])) ExitProcess(1);
    NTSTATUS status = LsaLookupSids(policy, 2, sids, &domains, &names);
    if (status != 0x107 || !domains || !names || names[0].Use != SidTypeWellKnownGroup ||
        names[0].Name.Length != 16 || names[0].Name.Buffer[0] != L'E' || names[1].Use != SidTypeUnknown ||
        names[1].DomainIndex != -1) ExitProcess(2);
    LsaFreeMemory(domains);
    LsaFreeMemory(names);
    domains = 0; names = 0;
    if (LsaLookupSids(policy, 1, sids, &domains, &names) || !names || names[0].Name.Length != 16) ExitProcess(3);
    LsaFreeMemory(domains);
    LsaFreeMemory(names);
    FreeSid(sids[0]); FreeSid(sids[1]);
    LsaClose(policy);
    ExitProcess(0);
}
