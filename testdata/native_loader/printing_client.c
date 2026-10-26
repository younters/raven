#include <windows.h>
#include <winspool.h>
#include <commdlg.h>

#define CHECK(test, code) do { if (!(test)) ExitProcess(code); } while (0)

void raven_entry(void)
{
	DWORD size=0, count=0;
	CHECK(!EnumPrintersW(PRINTER_ENUM_LOCAL,0,4,0,0,&size,&count) && size>0,1);
	BYTE *list=GlobalAlloc(GPTR,size);
	CHECK(list && EnumPrintersW(PRINTER_ENUM_LOCAL,0,4,list,size,&size,&count) && count>0,2);
	GlobalFree(list);
	HANDLE printer=0;
	CHECK(OpenPrinterW(L"Raven PDF",&printer,0),3);
	DWORD needed=0;
	CHECK(!GetPrinterW(printer,2,0,0,&needed) && needed>sizeof(PRINTER_INFO_2W),33);
	PRINTER_INFO_2W *info=GlobalAlloc(GPTR,needed);
	CHECK(info && GetPrinterW(printer,2,(BYTE*)info,needed,&needed) && info->pPrinterName && info->pDriverName && info->pPrintProcessor,34);
	GlobalFree(info);
	CHECK(DeviceCapabilitiesW(L"Raven PDF",0,DC_PAPERS,0,0)==2,35);
	LONG bytes=DocumentPropertiesW(0,printer,L"Raven PDF",0,0,0);
	CHECK(bytes==sizeof(DEVMODEW),4);
	DEVMODEW *mode=GlobalAlloc(GPTR,bytes);
	CHECK(mode && DocumentPropertiesW(0,printer,L"Raven PDF",mode,0,DM_OUT_BUFFER)==IDOK,5);
	CHECK(mode->dmPrintQuality==300 && mode->dmCopies==1,6);
	CHECK(ClosePrinter(printer),7);
	PRINTDLGW dialog={0};dialog.lStructSize=sizeof(dialog);dialog.Flags=PD_RETURNDEFAULT|PD_RETURNDC;
	CHECK(PrintDlgW(&dialog) && dialog.hDC && dialog.hDevMode && dialog.hDevNames,29);
	CHECK(GetDeviceCaps(dialog.hDC,LOGPIXELSX)==300 && GlobalSize(dialog.hDevMode)>=sizeof(DEVMODEW),30);
	DeleteDC(dialog.hDC);GlobalFree(dialog.hDevMode);GlobalFree(dialog.hDevNames);
	PAGESETUPDLGA page={0};page.lStructSize=sizeof(page);page.Flags=PSD_RETURNDEFAULT|PSD_INHUNDREDTHSOFMILLIMETERS;
	CHECK(PageSetupDlgA(&page) && page.hDevMode && page.ptPaperSize.x>20000,31);
	GlobalFree(page.hDevMode);GlobalFree(page.hDevNames);
	PRINTDLGEXW extended={0};extended.lStructSize=sizeof(extended);extended.Flags=PD_RETURNDEFAULT|PD_RETURNDC;
	CHECK(PrintDlgExW(&extended)==S_OK && extended.hDC && extended.dwResultAction==PD_RESULT_PRINT,32);
	DeleteDC(extended.hDC);GlobalFree(extended.hDevMode);GlobalFree(extended.hDevNames);
	HDC dc=CreateDCW(L"WINSPOOL",L"Raven PDF",0,mode);
	mode->dmFields |= DM_ORIENTATION; mode->dmOrientation=DMORIENT_LANDSCAPE;
	HDC rotated=CreateDCW(L"WINSPOOL",L"Raven PDF",0,mode);
	CHECK(rotated && GetDeviceCaps(rotated,PHYSICALWIDTH)>GetDeviceCaps(rotated,PHYSICALHEIGHT),36);
	DeleteDC(rotated);
	GlobalFree(mode);
	CHECK(dc && GetDeviceCaps(dc,TECHNOLOGY)==DT_RASPRINTER,8);
	CHECK(GetDeviceCaps(dc,LOGPIXELSX)==300 && GetDeviceCaps(dc,LOGPIXELSY)==300,9);
	CHECK(GetDeviceCaps(dc,HORZRES)>2000 && GetDeviceCaps(dc,VERTRES)>3000,10);
	CHECK(GetDeviceCaps(dc,PHYSICALWIDTH)==GetDeviceCaps(dc,HORZRES) && GetDeviceCaps(dc,PHYSICALOFFSETX)==0,11);
	CHECK(SetMapMode(dc,MM_TEXT)==MM_TEXT,12);
	CHECK(EndPage(dc)==0,13);
	DOCINFOW doc={sizeof(doc),L"Raven printer contract",L"C:\\printing.pdf",0,0};
	CHECK(StartDocW(dc,&doc)>0,14);
	CHECK(StartDocW(dc,&doc)==0,15);
	LOGFONTW lf={0};lf.lfHeight=-50;
	HFONT font=CreateFontIndirectW(&lf);CHECK(font,16);SelectObject(dc,font);
	HPEN pen=CreatePen(PS_SOLID,5,RGB(0,70,200));CHECK(pen,17);SelectObject(dc,pen);
	HDC memory=CreateCompatibleDC(0);
	BITMAPINFO bi={0};bi.bmiHeader.biSize=40;bi.bmiHeader.biWidth=32;bi.bmiHeader.biHeight=-32;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
	DWORD *pixels=0;HBITMAP bitmap=CreateDIBSection(memory,&bi,DIB_RGB_COLORS,(void**)&pixels,0,0);
	CHECK(bitmap && memory,18);HGDIOBJ old=SelectObject(memory,bitmap);
	for(int y=0;y<32;++y)for(int x=0;x<32;++x)pixels[y*32+x]=((x/4+y/4)&1)?0x00cc2200:0x0000bb22;
	for(int page=0;page<2;++page){
		CHECK(StartPage(dc)>0,20);
		CHECK(TextOutW(dc,150,150,page?L"Raven PDF page two":L"Raven PDF page one",18),21);
		CHECK(MoveToEx(dc,150,250,0) && LineTo(dc,1800,250),22);
		CHECK(StretchBlt(dc,150,350,320,320,memory,0,0,32,32,SRCCOPY),23);
		CHECK(EndPage(dc)>0,24);
	}
	CHECK(EndDoc(dc)>0,25);
	CHECK(DeleteDC(dc),26);DeleteObject(font);DeleteObject(pen);
	SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);
	// ANSI entry points and AbortDoc leave no output behind.
	dc=CreateDCA("WINSPOOL","Raven PDF",0,0);CHECK(dc,27);
	DOCINFOA abort={sizeof(abort),"abort","C:\\aborted.pdf",0,0};
	CHECK(StartDocA(dc,&abort)>0 && StartPage(dc)>0 && AbortDoc(dc)>0 && DeleteDC(dc),28);
	HANDLE existing=CreateFileW(L"C:\\existing.pdf",GENERIC_WRITE,0,0,CREATE_ALWAYS,0,0);DWORD written=0;
	CHECK(existing!=INVALID_HANDLE_VALUE && WriteFile(existing,"keep",4,&written,0) && CloseHandle(existing),37);
	dc=CreateDCW(L"WINSPOOL",L"Raven PDF",0,0);doc.lpszOutput=L"C:\\existing.pdf";
	CHECK(dc && StartDocW(dc,&doc)>0 && AbortDoc(dc)>0 && DeleteDC(dc),38);
	ExitProcess(0);
}
