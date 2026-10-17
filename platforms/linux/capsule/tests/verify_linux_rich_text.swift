import AppKit

// Reads only the explicit Linux fixture export; never touches the user's store.
@main struct VerifyLinuxRichText {
    static func main() throws {
        let data = try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))
        let fixtures = try JSONSerialization.jsonObject(with: data) as! [[String: Any]]
        for fixture in fixtures {
            guard let bytes = Data(base64Encoded: fixture["asset"] as! String),
                  let rich = NSAttributedString(rtfd: bytes, documentAttributes: nil),
                  rich.string == fixture["text"] as? String,
                  CapsuleRichTextProjection.markdown(from: rich) == fixture["markdown"] as? String else {
                throw NSError(domain: "Linux RTFD failed native readback", code: 1)
            }
            var attachments: [String] = []
            rich.enumerateAttribute(.attachment,in:NSRange(location:0,length:rich.length)) { value,_,_ in
                if let attachment = value as? NSTextAttachment, let bytes = attachment.fileWrapper?.regularFileContents {
                    attachments.append(bytes.base64EncodedString())
                }
            }
            guard attachments.sorted() == (fixture["attachments"] as! [String]).sorted() else {
                throw NSError(domain:"Linux RTFD lost native attachment bytes",code:2)
            }
            print("AppKit decoded Linux RTFD:", fixture["name"]!)
        }
    }
}
