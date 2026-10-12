import Foundation

/// Characters every app draws at one width, so a text picture keeps its frame
/// when it is sent line by line into a proportional chat font.
public enum TextArtStyle: String, Codable, CaseIterable, Identifiable {
    /// Colour squares: all emoji share one width.
    case blocks
    /// Any characters (Chinese, symbols, emoji…), full-width space as background.
    /// Stored as "hanzi" so earlier choices keep working.
    case hanzi
    public var id: String { rawValue }
    public static let palette: [Character] = ["⬛", "⬜", "🟥", "🟧", "🟨", "🟩", "🟦", "🟪", "🟫"]
    public var background: Character { self == .blocks ? "⬜" : "\u{3000}" }
    /// Whether a character may appear in the grid.
    public func allows(_ c: Character) -> Bool {
        switch self {
        case .blocks: return Self.palette.contains(c)
        case .hanzi:
            // Everything visible, plus spaces; only line breaks, tabs and controls are refused.
            return !c.isNewline && c != "\t" && !c.unicodeScalars.allSatisfy { $0.properties.generalCategory == .control || $0.properties.generalCategory == .format }
        }
    }
}

public struct TextArtOptions: Codable, Equatable {
    public var style: TextArtStyle = .blocks
    /// Cells per line and number of lines.
    public var width = 10
    public var height = 10
    public static let sizes = [8, 10, 12, 14]
    public init() {}
    private enum CodingKeys: String, CodingKey { case style, width, height }
    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        style = (try? values.decode(TextArtStyle.self, forKey: .style)) ?? .blocks
        width = min(20, max(4, (try? values.decode(Int.self, forKey: .width)) ?? 10))
        height = min(20, max(4, (try? values.decode(Int.self, forKey: .height)) ?? 10))
    }
}

public enum TextArt {
    public static func instruction(options: TextArtOptions) -> String {
        let w = options.width, h = options.height
        let cells: String
        switch options.style {
        case .blocks:
            cells = "只能使用这 9 个方块字符，每个字符是一格：⬛黑 ⬜白 🟥红 🟧橙 🟨黄 🟩绿 🟦蓝 🟪紫 🟫棕。背景用 ⬜。"
        case .hanzi:
            cells = "可以使用任何字符：汉字、全角符号与标点、字母、emoji 都行，每个字符算一格。为了在聊天软件里对齐，优先用全角字符（汉字、全角符号、emoji），用笔画疏密不同的字表现明暗与轮廓，背景用全角空格（　，U+3000）。"
        }
        return """
        你是像素画师。把用户描述的东西画成 \(w) 格宽、\(h) 行高的文字画。
        要求：
        1. \(cells)
        2. 必须正好 \(h) 行，每行正好 \(w) 格，行与行上下对齐，构成一个稳定的矩形画框。
        3. 主体居中、轮廓清楚，一眼能认出画的是什么。
        4. 只输出画本身：不要标题、解释、代码块标记、行号，也不要空行。
        """
    }
    /// Forces any reply onto the grid: exactly `height` lines of exactly `width` allowed
    /// cells, so every line sent keeps the frame. Stray text and fences are dropped.
    public static func normalize(_ reply: String, options: TextArtOptions) -> [String] {
        let style = options.style
        // Code fences and lead-in lines ("这是一只猫：") are not part of the picture.
        let lines = reply.split(whereSeparator: \.isNewline).filter { line in
            let t = line.trimmingCharacters(in: .whitespaces)
            return !t.hasPrefix("```") && !(style == .hanzi && (t.hasSuffix(":") || t.hasSuffix("：")))
        }
        var rows: [[Character]] = lines.compactMap { line in
            var cells: [Character] = []
            for c in line {
                if style.allows(c) { cells.append(c) }
                else if c == " " && style == .hanzi { cells.append(style.background) }
            }
            return cells.isEmpty ? nil : cells
        }
        // A reply that also explains itself would add short rows; keep the picture's run.
        if rows.count > options.height { rows = Array(rows.prefix(options.height)) }
        let blank = Array(repeating: style.background, count: options.width)
        let fitted = rows.map { row in Array(row.prefix(options.width)) + Array(repeating: style.background, count: max(0, options.width - row.count)) }
        let top = (options.height - fitted.count) / 2
        let grid = Array(repeating: blank, count: max(0, top)) + fitted + Array(repeating: blank, count: max(0, options.height - fitted.count - top))
        return grid.map { String($0) }
    }
}
