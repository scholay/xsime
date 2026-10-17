import AppKit
import CryptoKit

// Compile with the production CapsuleStructure, BulletEditor and RichTextProjection
// sources. This exporter reads only these fixed examples, never the user's store.
@main struct NativeNotesFixtures {
    static func main() throws {
        let root=URL(fileURLWithPath: CommandLine.arguments[1])
        let samples=[
            "## 登录\n- 用户名：isaac\n- 密码: hidden\n- https://example.com\n普通一行\n> 引用\n",
            "- [ ] 未完成\n  - [x] 已完成\n1. 有序\n002) 标签: value\n1000. 不去标记\n",
            "# 标题\n```swift\nlet a = 1\nlet b = 2\n```\n## 尾组\n",
            "~~~\nfirst\n\n", "- "+String(repeating:"标",count:24)+"：值\n- "+String(repeating:"标",count:25)+"：句子\n",
            "- keyboard: safe\n- API key: secret\n- 密码：\n- [X] done\r\n中文\r\n",
            "- 标签： e\u{301} 👩🏽‍💻 \n", "```\n```", "````\ncode\n````", "١٢. Arabic\nⅣ) Roman\n². superscript\n中文"
        ]
        let grammar=samples.map { body -> [String:Any] in
            let sections=CapsuleEntryGrammar.sections(body: body)
            return ["body":body,"summary":CapsuleEntryGrammar.fallbackSummary(body: body) as Any? ?? NSNull(),
                    "sections":sections.map { section -> [String:Any] in
                        ["heading":section.heading ?? "", "rows":section.rows.map { row -> [String:Any] in
                            switch row {
                            case .field(let field): return ["type":"field","label":field.label,"value":field.value,"secret":field.isSecret]
                            case .line(let text): return ["type":"line","label":"","value":text,"secret":false]
                            case .code(let text): return ["type":"code","label":"","value":text,"secret":false]
                            }
                        }]
                    }
            ]
        }
        let headers=[
            ["tags:","  - work","  - 中文","owner: 'O''Brien'","\"项目名\": demo # note","nested:","  name: sample","  key: value"],
            ["owner: first","owner: second"], ["- orphan"], ["description: |","  first","  second"],
            ["aliases: [a, b]","网址: https://example.com","密码: test"]
        ].map { lines -> [String:Any] in
            guard let header=try? CapsuleFrontMatter.parse(lines) else {return ["lines":lines,"valid":false]}
            return ["lines":lines,"valid":true,"preserved":header.preservedLines,
                    "fields":header.userFields.map { ["label":$0.label,"value":$0.value,"secret":$0.isSecret] }]
        }
        let plain=NSFont.monospacedSystemFont(ofSize:12,weight:.regular)
        let rich=NSMutableAttributedString(string:"中文 Hello\n",attributes:[.font:plain])
        rich.append(NSAttributedString(string:"Bold",attributes:[.font:NSFont.boldSystemFont(ofSize:12)]))
        rich.append(NSAttributedString(string:" italic",attributes:[.font:NSFontManager.shared.convert(plain,toHaveTrait:.italicFontMask)]))
        rich.append(NSAttributedString(string:" 👩🏽‍💻 e\u{301}\nLink",attributes:[.font:plain,.link:URL(string:"https://example.com/path?q=1")!]))
        let richData=rich.rtfd(from:NSRange(location:0,length:rich.length),documentAttributes:[:])!
        let image=NSMutableAttributedString(string:"Title\n",attributes:[.font:plain])
        let bitmap = NSBitmapImageRep(bitmapDataPlanes:nil,pixelsWide:32,pixelsHigh:24,bitsPerSample:8,samplesPerPixel:4,hasAlpha:true,isPlanar:false,colorSpaceName:.deviceRGB,bytesPerRow:0,bitsPerPixel:0)!
        for y in 0..<24 { for x in 0..<32 { let at = y * bitmap.bytesPerRow + x * 4; bitmap.bitmapData![at] = 26; bitmap.bitmapData![at+1] = 179; bitmap.bitmapData![at+2] = 115; bitmap.bitmapData![at+3] = 255 } }
        let png=bitmap.representation(using:.png,properties:[:])!
        let file=FileWrapper(regularFileWithContents:png);file.preferredFilename="示例 image.png"
        image.append(NSAttributedString(attachment:NSTextAttachment(fileWrapper:file)))
        image.append(NSAttributedString(string:"\nCaption",attributes:[.font:plain]))
        let images=image.rtfd(from:NSRange(location:0,length:image.length),documentAttributes:[:])!
        let decodedRich=NSAttributedString(rtfd:richData,documentAttributes:nil)!
        let decodedImage=NSAttributedString(rtfd:images,documentAttributes:nil)!
        let texts=["中文", "e\u{301}","👩🏽‍💻", "🇯🇵", "\r\n", String(repeating:"标",count:256)]
        var hashes:[String:String]=[:]
        for name in ["CapsuleStructure.swift","CapsuleBulletEditor.swift","CapsuleRichTextProjection.swift"] {
            let data=try Data(contentsOf:root.appendingPathComponent("Sources/RimeBuffer/"+name))
            hashes[name]=SHA256.hash(data:data).map{String(format:"%02x",$0)}.joined()
        }
        let result:[String:Any] = ["sources":hashes,"grammar":grammar,"headers":headers,
            "summaries":[" first   line \n second\tpart ","  中文\u{2028} line ",String(repeating:"e\u{301}",count:121)].map { ["input":$0,"normalized":CapsuleSummaryRules.normalized($0) ?? ""] },
            "characters":texts.map { ["text":$0,"count":$0.count,"prefix":String($0.prefix(1))] },
            "bullets":["- [ ] task","  - [x] task","\t- [X] task","plain"].map { ["line":$0,"toggle":CapsuleBulletMarkdown.toggledLine($0) as Any? ?? NSNull(),"indent":CapsuleBulletMarkdown.indentedLine($0,outdent:false),"outdent":CapsuleBulletMarkdown.indentedLine($0,outdent:true)] },
            "rich":[["name":"text","asset":richData.base64EncodedString(),"text":decodedRich.string,"markdown":CapsuleRichTextProjection.markdown(from:decodedRich)],
                    ["name":"image","asset":images.base64EncodedString(),"text":decodedImage.string,"markdown":CapsuleRichTextProjection.markdown(from:decodedImage)]]]
        let data=try JSONSerialization.data(withJSONObject:result,options:[.prettyPrinted,.sortedKeys,.withoutEscapingSlashes])
        try data.write(to:URL(fileURLWithPath:CommandLine.arguments[2]))
    }
}
