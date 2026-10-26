import AppKit
import PDFKit
let path = CommandLine.arguments[1]
guard let pdf = PDFDocument(url: URL(fileURLWithPath: path)), pdf.pageCount == 2 else { fatalError("expected two PDF pages") }
for n in 0..<2 {
    let page = pdf.page(at: n)!
    let bounds = page.bounds(for: .mediaBox)
    guard bounds.width >= 590 && bounds.width <= 620 && bounds.height >= 790 && bounds.height <= 845 else { fatalError("incorrect paper size") }
    let image = page.thumbnail(of: NSSize(width: 612, height: 850), for: .mediaBox)
    let bitmap = NSBitmapImageRep(data: image.tiffRepresentation!)!
    var dark = 0
    for y in 0..<bitmap.pixelsHigh { for x in 0..<bitmap.pixelsWide {
        let c = bitmap.colorAt(x: x, y: y)!.usingColorSpace(.deviceRGB)!
        if c.redComponent < 0.8 || c.greenComponent < 0.8 || c.blueComponent < 0.8 { dark += 1 }
    } }
    guard dark > 1000 else { fatalError("page is blank") }
    try bitmap.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: path + "-page-\(n+1).png"))
}
print("PDF: two nonblank pages, expected paper dimensions; rendered both pages")
