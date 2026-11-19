# Printing

Windows programs can create a printer DC with `CreateDC`/`CreateIC` (A/W), or obtain one from `PrintDlg`/`PrintDlgEx` with
`PD_RETURNDC`/`PD_RETURNIC`. The host enumerates macOS queues and adds **Raven PDF**. The default is the macOS default printer,
falling back to Raven PDF when there is none. `OpenPrinter`, `ClosePrinter`, `GetPrinter` (levels 1/2/4/5), `EnumPrinters`
(levels 1/2/4/5), `GetDefaultPrinter`, `DocumentProperties`, and basic `DeviceCapabilities` are provided at both guest widths.

## Page rendering

A printer DC owns a 32-bit, top-down, 300-DPI bitmap for the printable area. Physical dimensions and imageable bounds come from
`NSPrintInfo`; Raven PDF uses the default paper with no hardware margins. DEVMODE carries paper dimensions, Letter/A4 paper IDs,
orientation, copies, grayscale/color and 300-DPI resolution. `GetDeviceCaps` reports printable pixels, paper pixels, physical offsets,
physical millimetres and printer DPI independently of the desktop DPI. The same GDI text, clipping, pens, paths and bitmap code
used on screen draws the printed page. `StartPage` clears it white, and `EndPage` writes an image into a CoreGraphics PDF page.
Each job uses one staged PDF file; `EndDoc` commits/submits it, and `AbortDoc`/DC destruction removes the staging file. An aborted
job never truncates an existing output file. Page storage has the existing GDI bitmap limit of 64 MiB.

The shared Skia build remains unchanged (`skia_enable_pdf=false`). Enabling PDF in a separate Skia output would supply a PDF canvas,
but Raven's GDI painter, raster operations and some text paths modify bitmap pixels directly. Merely selecting a Skia PDF canvas
would silently omit these operations. Raster pages preserve all existing drawing behavior, including destination-dependent raster
operations. **Text is rasterized, not searchable or selectable.** A future vector backend needs an operation recorder with raster
fallback, rather than a build-argument change alone.

**Mapping limitation:** only the existing `MM_TEXT` mapping is supported (including window/viewport origins and clipping).
Other modes still fail explicitly; this implementation does not add scaled/anisotropic mappings or world-transform rendering.

## Panels and submission

An AppKit modal chooser exposes both real queues and Raven PDF, with copies, page range, landscape and grayscale controls.
Real queues then use `NSPrintPanel`; page setup uses `NSPageLayout`. The chooser is necessary because an application-defined
virtual printer is not an installed macOS queue that `NSPrintPanel` can enumerate. API handlers run synchronously on the desktop's
AppKit thread, so no cross-thread wait or change to the desktop event pump is required. The panels are application-modal, not
sheets attached to a particular Windows HWND. PRINTDLG, PRINTDLGEX and PAGESETUPDLG fields are marshalled separately at each width;
DEVMODE and DEVNAMES blocks use the normal GlobalAlloc allocator. Hooks, custom dialog templates and PrintDlgEx COM callbacks
are not implemented.

`EndDoc` submits physical queues with `PMPrinterPrintWithFile`, using the selected settings and page format. A Raven PDF job saves
to `DOCINFO.lpszOutput` when present (resolved through the program's Windows drive mappings); otherwise it saves a uniquely named
`Raven-<pid>-<job>.pdf` in the user's Downloads folder. Choosing Save as PDF in a native print panel retains the selected output URL.
Print-to-file prompting through `PD_PRINTTOFILE`/the special output name `FILE:` is not implemented; callers can name an output
file explicitly or select Raven PDF. AbortProc callbacks and asynchronous spooler status notifications are not implemented.

## Verification safety

Set `RAVEN_PRINT_PDF_ONLY=1` for all automated/manual verification. It hides physical printers, selects Raven PDF as default,
and rejects physical document creation at the backend boundary. The fixture runner always sets this variable and uses its own
prefix. Never use an installed physical queue for verification.

Build normally; no shared components or shared Skia archive need modification. Run tests through the machine-wide lock:

```sh
/tmp/raven-ctest-lock.sh printing ctest --test-dir build -j1 -R native_printing_pdf --output-on-failure
```

The x64 and x86 PE fixtures cover printer enumeration, printer information, default dialog structures without UI, DEVMODE,
landscape capabilities, printer DPI and physical geometry, two pages of text/lines/a stretched bitmap, and abort cleanup including
preservation of an existing output. The host verifies two PDF pages, paper dimensions and nonblank renderings, and saves PNGs.
The fixtures never submit to a physical queue.
