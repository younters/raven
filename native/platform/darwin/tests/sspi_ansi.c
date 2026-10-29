#define SECURITY_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <security.h>
#include <schannel.h>

int raven_entry(void)
{
	PSecurityFunctionTableA table = InitSecurityInterfaceA();
	CredHandle credential;
	CtxtHandle context;
	TimeStamp expiry;
	SecPkgInfoA *package = NULL;
	if (!table || table != InitSecurityInterfaceA() || table->dwVersion < 1) return 1;
	if (table->QuerySecurityPackageInfoA("Negotiate", &package) || !package || !package->cbMaxToken ||
		lstrcmpA(package->Name, "Negotiate")) return 2;
	table->FreeContextBuffer(package);
	if (table->AcquireCredentialsHandleA("caf\xe9","Negotiate",SECPKG_CRED_OUTBOUND,NULL,NULL,NULL,NULL,&credential,&expiry)) return 3;
	if (table->FreeCredentialsHandle(&credential)) return 4;
	SCHANNEL_CRED settings = {0};
	settings.dwVersion = SCHANNEL_CRED_VERSION;
	if (table->AcquireCredentialsHandleA(NULL,"Schannel",SECPKG_CRED_OUTBOUND,NULL,&settings,NULL,NULL,&credential,&expiry)) return 5;
	SecBuffer token = {0,SECBUFFER_TOKEN,NULL};
	SecBufferDesc output = {SECBUFFER_VERSION,1,&token};
	ULONG attributes = 0;
	SECURITY_STATUS result = table->InitializeSecurityContextA(&credential,NULL,"caf\xe9.example",
		ISC_REQ_ALLOCATE_MEMORY|ISC_REQ_STREAM,0,SECURITY_NATIVE_DREP,NULL,0,&context,&output,&attributes,&expiry);
	if (result != SEC_I_CONTINUE_NEEDED || token.cbBuffer < 5 || !token.pvBuffer) return 6;
	// The TLS SNI must contain UTF-8 converted from the ANSI CP1252 target.
	const unsigned char expected[] = "caf\xc3\xa9.example";
	unsigned matched = 0;
	for (unsigned i=0; i+sizeof(expected)-1 <= token.cbBuffer; ++i) {
		unsigned n=0;
		while (n<sizeof(expected)-1 && ((unsigned char *)token.pvBuffer)[i+n]==expected[n]) ++n;
		if (n==sizeof(expected)-1) matched=1;
	}
	if (!matched) return 8;
	if (!table->QueryCredentialsAttributesA || !table->ImportSecurityContextA || !table->AddCredentialsA ||
		!table->SetContextAttributesA || !table->SetCredentialsAttributesA || !table->ChangeAccountPasswordA) return 9;
	if (table->QueryCredentialsAttributesA(&credential, 0, NULL) != SEC_E_UNSUPPORTED_FUNCTION ||
		table->ImportSecurityContextA("Schannel", NULL, NULL, &context) != SEC_E_UNSUPPORTED_FUNCTION ||
		table->AddCredentialsA(&credential, "caf\xe9", "Schannel", SECPKG_CRED_OUTBOUND, NULL, NULL, NULL, &expiry) != SEC_E_UNSUPPORTED_FUNCTION ||
		table->SetContextAttributesA(&context, 0, NULL, 0) != SEC_E_UNSUPPORTED_FUNCTION ||
		table->SetCredentialsAttributesA(&credential, 0, NULL, 0) != SEC_E_UNSUPPORTED_FUNCTION ||
		table->ChangeAccountPasswordA("Negotiate", "domain", "caf\xe9", "old", "new", FALSE, 0, NULL) != SEC_E_UNSUPPORTED_FUNCTION) return 10;
	table->FreeContextBuffer(token.pvBuffer);
	if (table->DeleteSecurityContext(&context) || table->FreeCredentialsHandle(&credential)) return 7;
	return 0;
}
