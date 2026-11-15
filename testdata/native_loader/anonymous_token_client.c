#include <windows.h>

/* NtImpersonateAnonymousToken and advapi32's ImpersonateAnonymousToken: the thread impersonates ANONYMOUS LOGON (S-1-5-7), the
 * program's own token is left as it was, and RevertToSelf takes the impersonation away again. A closed handle and a thread handle
 * without THREAD_IMPERSONATE are refused.
 *
 * Documented on Microsoft Learn: S-1-5-7 (Well-known SIDs); THREAD_IMPERSONATE, RevertToSelf and no Everyone by default
 * (ImpersonateAnonymousToken); a logon SID is a group with SE_GROUP_LOGON_ID (TOKEN_GROUPS); tokens with different SIDs are not equal
 * (NtCompareTokens); a NULL token means the impersonation token (CheckTokenMembership); GetUserName and GetUserNameEx name the client
 * the thread impersonates (GetUserName, GetUserNameEx); TokenElevationTypeDefault means no linked token (TOKEN_ELEVATION_TYPE), and a
 * token without elevated privileges reports TokenIsElevated 0 (TOKEN_ELEVATION). Raven's choice, checked here so a change to it is
 * deliberate: the SecurityImpersonation level, untrusted integrity, a logon session apart from the program's, no linked token, the
 * names ANONYMOUS LOGON and NT AUTHORITY\ANONYMOUS LOGON, and no display name. */
typedef LONG (WINAPI *ImpersonateAnonymous)(HANDLE);
typedef LONG (WINAPI *CompareTokens)(HANDLE, HANDLE, BOOLEAN *);
typedef BOOLEAN (WINAPI *UserNameEx)(int, WCHAR *, ULONG *);

static int same_wide(const WCHAR *one, const WCHAR *other)
{
	while (*one && *one == *other)
		++one, ++other;
	return *one == *other;
}

static int same_narrow(const char *one, const char *other)
{
	while (*one && *one == *other)
		++one, ++other;
	return *one == *other;
}

/* The program's user name, read before the thread impersonates. */
static WCHAR program_name[257];

/* What the calling thread's user is called: GetUserNameW and GetUserNameA give the name and count its terminator. */
static BOOL user_name_is(const WCHAR *wide, const char *narrow, DWORD length)
{
	WCHAR name[257];
	char name_narrow[257];
	DWORD size = 257, size_narrow = 257;
	return GetUserNameW(name, &size) && size == length && same_wide(name, wide) && GetUserNameA(name_narrow, &size_narrow) &&
		size_narrow == length && (!narrow || same_narrow(name_narrow, narrow));
}

/* Names and elevation while the thread impersonates the anonymous logon; 0 when they are as expected, otherwise the step. */
static UINT anonymous_names(HANDLE token, PSID user)
{
	UserNameEx name_ex = (UserNameEx)GetProcAddress(LoadLibraryA("secur32.dll"), "GetUserNameExW");
	TOKEN_ELEVATION elevation = {1};
	TOKEN_ELEVATION_TYPE elevation_type = 0;
	WCHAR name[64], domain[64];
	DWORD length = 0, name_size = 64, domain_size = 64;
	ULONG size = 64;
	SID_NAME_USE use = 0;
	if (!GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &length) || elevation.TokenIsElevated)
		return 10;
	/* No linked token: Raven's choice; TokenElevationTypeDefault is what that means. */
	if (!GetTokenInformation(token, TokenElevationType, &elevation_type, sizeof elevation_type, &length) ||
		elevation_type != TokenElevationTypeDefault)
		return 11;
	/* GetUserName names the impersonated client; the spelling is Raven's choice. */
	if (!user_name_is(L"ANONYMOUS LOGON", "ANONYMOUS LOGON", 16))
		return 12;
	if (!name_ex || !name_ex(2 /* NameSamCompatible */, name, &size) || size != 28 || !same_wide(name, L"NT AUTHORITY\\ANONYMOUS LOGON"))
		return 13;
	/* No display name (Raven's choice): ERROR_NONE_MAPPED, the documented error for a format the name is not available in. */
	size = 64;
	SetLastError(0);
	if (name_ex(3 /* NameDisplay */, name, &size) || GetLastError() != ERROR_NONE_MAPPED)
		return 14;
	/* The token's user looks up under the same name. */
	if (!LookupAccountSidW(NULL, user, name, &name_size, domain, &domain_size, &use) || !same_wide(name, L"ANONYMOUS LOGON") ||
		!same_wide(domain, L"NT AUTHORITY") || use != SidTypeWellKnownGroup)
		return 15;
	return 0;
}

static int sid_is(const SID *sid, BYTE authority, DWORD first)
{
	return sid->Revision == 1 && sid->SubAuthorityCount == 1 && sid->IdentifierAuthority.Value[5] == authority && sid->SubAuthority[0] == first;
}

static union {
	TOKEN_USER user;
	TOKEN_MANDATORY_LABEL label;
	TOKEN_GROUPS groups;
	BYTE bytes[512];
} buffer;

/* The program's own user and logon SID, read from the process token. */
static union { TOKEN_USER user; BYTE bytes[256]; } process_user;
static union { TOKEN_GROUPS groups; BYTE bytes[256]; } process_logon;

static BOOL read_process_identity(void)
{
	HANDLE own;
	DWORD length = 0;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own))
		return FALSE;
	BOOL read = GetTokenInformation(own, TokenUser, &process_user, sizeof process_user, &length) &&
		GetTokenInformation(own, TokenLogonSid, &process_logon, sizeof process_logon, &length) && process_logon.groups.GroupCount == 1;
	CloseHandle(own);
	return read;
}

static SID everyone_sid = {SID_REVISION, 1, {SECURITY_WORLD_SID_AUTHORITY}, {SECURITY_WORLD_RID}};
static SID authenticated_sid = {SID_REVISION, 1, {SECURITY_NT_AUTHORITY}, {SECURITY_AUTHENTICATED_USER_RID}};

/* The group in the list with this SID, or NULL. */
static SID_AND_ATTRIBUTES *group_of(TOKEN_GROUPS *groups, PSID sid)
{
	for (DWORD index = 0; index < groups->GroupCount; ++index)
		if (EqualSid(groups->Groups[index].Sid, sid))
			return &groups->Groups[index];
	return NULL;
}

/* A descriptor owned by the program's user whose list allows the one SID. */
static BOOL allows(SECURITY_DESCRIPTOR *descriptor, ACL *acl, DWORD size, PSID owner, PSID sid)
{
	return InitializeSecurityDescriptor(descriptor, SECURITY_DESCRIPTOR_REVISION) && SetSecurityDescriptorOwner(descriptor, owner, FALSE) &&
		SetSecurityDescriptorGroup(descriptor, owner, FALSE) && InitializeAcl(acl, size, ACL_REVISION) &&
		AddAccessAllowedAce(acl, ACL_REVISION, 7, sid) && SetSecurityDescriptorDacl(descriptor, TRUE, acl, FALSE);
}

/* AccessCheck and CheckTokenMembership (with the handle and with NULL) agree with the groups the token reports: a list that allows
 * the program's user, Everyone, Authenticated Users or the program's logon SID does not let it in; one that allows ANONYMOUS LOGON
 * or the token's own logon SID does. 0 when they agree, otherwise the step that differs. */
static UINT access_checks(HANDLE token, PSID logon)
{
	static SID anonymous = {SID_REVISION, 1, {SECURITY_NT_AUTHORITY}, {SECURITY_ANONYMOUS_LOGON_RID}};
	GENERIC_MAPPING mapping = {1, 2, 4, 7};
	/* The program's logon SID is refused because the anonymous logon is a session of its own here (Raven's choice). */
	const struct { PSID sid; BOOL member; } cases[] = {
		{process_user.user.User.Sid, FALSE}, {&everyone_sid, FALSE}, {&authenticated_sid, FALSE}, {process_logon.groups.Groups[0].Sid, FALSE},
		{&anonymous, TRUE}, {logon, TRUE},
	};
	for (UINT index = 0; index < sizeof cases / sizeof *cases; ++index) {
		SECURITY_DESCRIPTOR descriptor;
		static BYTE acl[128];
		BYTE privileges[64];
		DWORD privileges_length = sizeof privileges, granted = 0;
		BOOL status = !cases[index].member, member = !cases[index].member;
		if (!allows(&descriptor, (ACL *)acl, sizeof acl, process_user.user.User.Sid, cases[index].sid))
			return 21;
		if (!AccessCheck(&descriptor, token, 1, &mapping, (PRIVILEGE_SET *)privileges, &privileges_length, &granted, &status))
			return 22;
		if (status != cases[index].member)
			return 30 + index;
		if (!CheckTokenMembership(token, cases[index].sid, &member) || member != cases[index].member)
			return 40 + index;
		member = !cases[index].member;
		if (!CheckTokenMembership(NULL, cases[index].sid, &member) || member != cases[index].member)
			return 50 + index;
	}
	return 0;
}

/* NtCompareTokens: a copy of the anonymous token is equal to it. The program's token cut down to the same privileges and given the same
 * integrity level still holds different SIDs, so it is not equal. */
static UINT comparisons(HANDLE token)
{
	CompareTokens compare = (CompareTokens)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtCompareTokens");
	HANDLE copy, own, cut;
	BOOLEAN equal = FALSE;
	DWORD length = 0;
	if (!compare)
		return 60;
	if (!DuplicateTokenEx(token, TOKEN_QUERY, NULL, SecurityImpersonation, TokenImpersonation, &copy))
		return 61;
	if (compare(token, copy, &equal) || !equal)
		return 62;
	if (!GetTokenInformation(copy, TokenUser, &buffer, sizeof buffer, &length) || !sid_is(buffer.user.User.Sid, 5, 7))
		return 63;
	CloseHandle(copy);
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &own) || !CreateRestrictedToken(own, DISABLE_MAX_PRIVILEGE, 0, NULL, 0,
		NULL, 0, NULL, &cut))
		return 64;
	CloseHandle(own);
	if (!GetTokenInformation(token, TokenIntegrityLevel, &buffer, sizeof buffer, &length) ||
		!SetTokenInformation(cut, TokenIntegrityLevel, &buffer, length))
		return 65;
	equal = TRUE;
	if (compare(token, cut, &equal) || equal)
		return 66;
	CloseHandle(cut);
	return 0;
}

/* The thread's token is the anonymous one; 0 when it is, otherwise the step that differs. */
static UINT impersonating_anonymous(void)
{
	HANDLE token;
	DWORD length = 0;
	if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, &token))
		return 1;
	UINT result = 0;
	TOKEN_TYPE type = 0;
	SECURITY_IMPERSONATION_LEVEL level = 0;
	static union { TOKEN_GROUPS groups; BYTE bytes[256]; } logon;
	SID_AND_ATTRIBUTES *group;
	if (!GetTokenInformation(token, TokenUser, &buffer, sizeof buffer, &length) || !sid_is(buffer.user.User.Sid, 5, 7))
		result = 2;
	else if (!GetTokenInformation(token, TokenType, &type, sizeof type, &length) || type != TokenImpersonation)
		result = 3;
	/* Raven's choice. */
	else if (!GetTokenInformation(token, TokenImpersonationLevel, &level, sizeof level, &length) || level != SecurityImpersonation)
		result = 4;
	/* Raven's choice. */
	else if (!GetTokenInformation(token, TokenIntegrityLevel, &buffer, sizeof buffer, &length) || !sid_is(buffer.label.Label.Sid, 16, 0))
		result = 5;
	else if (!GetTokenInformation(token, TokenLogonSid, &logon, sizeof logon, &length) || logon.groups.GroupCount != 1 ||
		EqualSid(logon.groups.Groups[0].Sid, process_logon.groups.Groups[0].Sid))
		result = 6;
	else if (!GetTokenInformation(token, TokenGroups, &buffer, sizeof buffer, &length))
		result = 7;
	/* Not Everyone, Authenticated Users or the program's user; the logon SID is one of the groups. */
	else if (group_of(&buffer.groups, &everyone_sid) || group_of(&buffer.groups, &authenticated_sid) ||
		group_of(&buffer.groups, process_user.user.User.Sid))
		result = 8;
	else if (!(group = group_of(&buffer.groups, logon.groups.Groups[0].Sid)) || (group->Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID)
		result = 9;
	else if (!GetTokenInformation(token, TokenUser, &buffer, sizeof buffer, &length))
		result = 16;
	else if ((result = anonymous_names(token, buffer.user.User.Sid)) == 0 && (result = access_checks(token, logon.groups.Groups[0].Sid)) == 0)
		result = comparisons(token);
	CloseHandle(token);
	return result;
}

static int not_impersonating(void)
{
	HANDLE token;
	return !OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) && GetLastError() == ERROR_NO_TOKEN;
}

/* A process started with a primary copy of the anonymous token: its own token, its user name and membership with no token handle are
 * the anonymous logon's (CreateProcessAsUserW; CheckTokenMembership uses the process token when the thread does not impersonate). */
static UINT anonymous_child(void)
{
	SID anonymous = {SID_REVISION, 1, {SECURITY_NT_AUTHORITY}, {SECURITY_ANONYMOUS_LOGON_RID}};
	WCHAR name[64];
	DWORD length = 0, size = 64;
	BOOL member = FALSE;
	HANDLE own;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own) || !GetTokenInformation(own, TokenUser, &buffer, sizeof buffer, &length) ||
		!sid_is(buffer.user.User.Sid, 5, 7))
		return 31;
	CloseHandle(own);
	if (!not_impersonating() || !GetUserNameW(name, &size) || size != 16 || !same_wide(name, L"ANONYMOUS LOGON"))
		return 32;
	if (!CheckTokenMembership(NULL, &anonymous, &member) || !member)
		return 33;
	if (!CheckTokenMembership(NULL, &everyone_sid, &member) || member)
		return 34;
	return 0;
}

/* The parent: impersonate the anonymous logon, make a primary copy of that token, go back to the program's own token and start this
 * program through CreateProcessAsUserW with the copy. 0 when the child reports the anonymous logon, otherwise the step. */
static UINT anonymous_process(ImpersonateAnonymous impersonate)
{
	HANDLE thread_token = NULL, primary = NULL;
	STARTUPINFOW startup;
	PROCESS_INFORMATION child;
	WCHAR command[512];
	DWORD length, code = 0;
	UINT result = 0;
	unsigned index;
	if (impersonate(GetCurrentThread()))
		return 1;
	if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, &thread_token) ||
		!DuplicateTokenEx(thread_token, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, NULL, SecurityImpersonation, TokenPrimary, &primary))
		result = 2;
	if (!RevertToSelf() || !not_impersonating())
		result = result ? result : 3;
	for (index = 0; index < sizeof startup; ++index)
		((BYTE *)&startup)[index] = 0;
	startup.cb = sizeof startup;
	command[0] = '"';
	length = GetModuleFileNameW(NULL, command + 1, 480);
	if (!result && (!length || length >= 480))
		result = 4;
	if (!result) {
		command[++length] = '"'; command[++length] = ' '; command[++length] = '-'; command[++length] = '-'; command[++length] = 'x';
		command[++length] = 0;
		if (!CreateProcessAsUserW(primary, NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child))
			result = 5;
		else {
			if (WaitForSingleObject(child.hProcess, 15000) != WAIT_OBJECT_0 || !GetExitCodeProcess(child.hProcess, &code)) {
				TerminateProcess(child.hProcess, 99);
				WaitForSingleObject(child.hProcess, 5000);
				result = 6;
			} else if (code)
				result = code;
			CloseHandle(child.hThread);
			CloseHandle(child.hProcess);
		}
	}
	if (primary)
		CloseHandle(primary);
	if (thread_token)
		CloseHandle(thread_token);
	return result;
}

UINT raven_entry(void)
{
	ImpersonateAnonymous impersonate = (ImpersonateAnonymous)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtImpersonateAnonymousToken");
	BOOL (WINAPI *win32)(HANDLE) = (void *)GetProcAddress(LoadLibraryA("advapi32.dll"), "ImpersonateAnonymousToken");
	DWORD program_length = 257;
	const WCHAR *line = GetCommandLineW();
	unsigned index;
	for (index = 0; line[index]; ++index)
		if (line[index] == '-' && line[index + 1] == '-' && line[index + 2] == 'x')
			return anonymous_child();
	if (!impersonate || !win32 || !read_process_identity() || !GetUserNameW(program_name, &program_length))
		return 1;
	if (!not_impersonating() || same_wide(program_name, L"ANONYMOUS LOGON"))
		return 2;
	if (impersonate(GetCurrentThread()))
		return 3;
	UINT step = impersonating_anonymous();
	if (step)
		return 100 + step;
	/* The process keeps its own user. */
	HANDLE own;
	DWORD length = 0;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own) || !GetTokenInformation(own, TokenUser, &buffer, sizeof buffer, &length) ||
		sid_is(buffer.user.User.Sid, 5, 7))
		return 4;
	CloseHandle(own);
	/* After RevertToSelf the thread's user is the program's again. */
	if (!RevertToSelf() || !not_impersonating() || !user_name_is(program_name, NULL, program_length))
		return 5;
	/* The Win32 call gives the same token. */
	if (!win32(GetCurrentThread()))
		return 6;
	step = impersonating_anonymous();
	if (step)
		return 200 + step;
	if (!RevertToSelf() || !not_impersonating())
		return 7;
	/* THREAD_SET_THREAD_TOKEN is not enough: STATUS_ACCESS_DENIED, ERROR_ACCESS_DENIED. THREAD_IMPERSONATE is. */
	HANDLE limited = OpenThread(THREAD_SET_THREAD_TOKEN | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
	if (!limited || (ULONG)impersonate(limited) != 0xc0000022U)
		return 8;
	SetLastError(0);
	if (win32(limited) || GetLastError() != ERROR_ACCESS_DENIED || !not_impersonating())
		return 9;
	CloseHandle(limited);
	HANDLE allowed = OpenThread(THREAD_IMPERSONATE, FALSE, GetCurrentThreadId());
	if (!allowed || impersonate(allowed) || impersonating_anonymous() || !RevertToSelf())
		return 10;
	/* A closed handle: STATUS_INVALID_HANDLE, ERROR_INVALID_HANDLE. */
	CloseHandle(allowed);
	if ((ULONG)impersonate(allowed) != 0xc0000008U)
		return 11;
	SetLastError(0);
	if (win32(allowed) || GetLastError() != ERROR_INVALID_HANDLE || !not_impersonating())
		return 12;
	/* A process started with a primary copy of the anonymous token is the anonymous logon; this one stays the program's user. */
	if ((step = anonymous_process(impersonate)) != 0)
		return 300 + step;
	if (!user_name_is(program_name, NULL, program_length))
		return 13;
	return 0;
}
