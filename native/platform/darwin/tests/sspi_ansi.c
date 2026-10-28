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
	if (table->AcquireCredentialsHandleA(NULL,"Negotiate",SECPKG_CRED_OUTBOUND,NULL,NULL,NULL,NULL,&credential,&expiry)) return 3;
	if (table->FreeCredentialsHandle(&credential)) return 4;
	SCHANNEL_CRED settings = {0};
	settings.dwVersion = SCHANNEL_CRED_VERSION;
	if (table->AcquireCredentialsHandleA(NULL,"Schannel",SECPKG_CRED_OUTBOUND,NULL,&settings,NULL,NULL,&credential,&expiry)) return 5;
	SecBuffer token = {0,SECBUFFER_TOKEN,NULL};
	SecBufferDesc output = {SECBUFFER_VERSION,1,&token};
	ULONG attributes = 0;
	SECURITY_STATUS result = table->InitializeSecurityContextA(&credential,NULL,"localhost",
		ISC_REQ_ALLOCATE_MEMORY|ISC_REQ_STREAM,0,SECURITY_NATIVE_DREP,NULL,0,&context,&output,&attributes,&expiry);
	if (result != SEC_I_CONTINUE_NEEDED || token.cbBuffer < 5 || !token.pvBuffer) return 6;
	table->FreeContextBuffer(token.pvBuffer);
	if (table->DeleteSecurityContext(&context) || table->FreeCredentialsHandle(&credential)) return 7;
	return 0;
}
