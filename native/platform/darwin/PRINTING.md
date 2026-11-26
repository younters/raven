# Printing

Windows programs can create a printer DC with `CreateDC`/`CreateIC` (A/W), or obtain one from `PrintDlg`/`PrintDlgEx` with
`PD_RETURNDC`/`PD_RETURNIC`. The host enumerates macOS queues and adds **Raven PDF**. The default is the macOS default printer,
falling back to Raven PDF when there is none. `OpenPrinter`, `ClosePrinter`, `GetPrinter` (levels 1/2/4/5), `EnumPrinters`
(levels 1/2/4/5), `GetDefaultPrinter`, `DocumentProperties`, and basic `DeviceCapabilities` are provided at both guest widths.

## Page rendering

A printer DC owns a 32-bit, top-down, 300-DPI bitmap for the printable area. Physical dimensions and imageable bounds come from
`NSPrintInfo`; Raven PDF uses the default paper with no hardware margins. DEVMODE carries paper dimensions, Letter/A4 paper IDs,
orientation, copies, grayscale/color and 300-DPI resolution. `GetDeviceCaps` reports printable pixels, paper pixels, physical offsets,
physical millimeters and printer DPI independently of the desktop DPI. The same GDI text, clipping, pens, paths and bitmap code
used on screen draws the printed page. `StartPage` clears it white, and `EndPage` writes an image into a CoreGraphics PDF page.
Each job uses one staged PDF file; `EndDoc` commits/submits it, and `AbortDoc`/DC destruction removes the staging file. An aborted
job never truncates an existing output file. Page storage has the existing GDI bitmap limit of 64 MiB.

Raster pages preserve destination-dependent GDI operations. See the [macOS limitations](../../../docs/macos/rosetta-and-fex.md#known-limitations)
for the supported mapping modes and print features.

## Panels and submission

An AppKit modal chooser exposes both real queues and Raven PDF, with copies, page range, landscape and grayscale controls.
Real queues then use `NSPrintPanel`; page setup uses `NSPageLayout`. The chooser is necessary because an application-defined
virtual printer is not an installed macOS queue that `NSPrintPanel` can enumerate. API handlers run synchronously on the desktop's
AppKit thread, so no cross-thread wait or change to the desktop event pump is required. The panels are application-modal. PRINTDLG, PRINTDLGEX and PAGESETUPDLG fields are marshalled separately at each width;
DEVMODE and DEVNAMES blocks use the normal GlobalAlloc allocator.

`EndDoc` submits physical queues with `PMPrinterPrintWithFile`, using the selected settings and page format. A Raven PDF job saves
to `DOCINFO.lpszOutput` when present (resolved through the program's Windows drive mappings); otherwise it saves a uniquely named
`Raven-<pid>-<job>.pdf` in the user's Downloads folder. Choosing Save as PDF in a native print panel retains the selected output URL.
## Tests

`RAVEN_PRINT_PDF_ONLY=1` hides physical printers, makes Raven PDF the default and refuses to create a document on a physical
queue. The PDF tests set it and use a test prefix. Set it for a manual PDF test to keep the job off physical queues.
`RAVEN_PRINT_NONE=1` presents a machine with no printers at all, not even Raven PDF.

```sh
ctest --test-dir build-macos -j1 -R native_printing_pdf --output-on-failure
```

The x64 and x86 fixtures cover printer enumeration and information, the default dialog structures without UI, DEVMODE, landscape
capabilities, printer DPI and physical geometry, two pages of text, lines and a stretched bitmap, and abort cleanup, including that
an existing output file survives. The host checks the two PDF pages, their paper size and that they are not blank.
