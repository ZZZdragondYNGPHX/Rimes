import Foundation

/// How the typed Buffer text shapes the poem. Acrostics are one mode among others.
public enum PoemMode: String, Codable, CaseIterable, Identifiable {
    case improvise, acrosticHead, acrosticTail
    public var id: String { rawValue }
}

/// Characters per line; `free` leaves line length to the chosen pattern.
public enum PoemLineLength: Int, Codable, CaseIterable, Identifiable {
    case free = 0, four = 4, five = 5, six = 6, seven = 7
    public var id: Int { rawValue }
}

/// A sentence pattern (句式). Built-ins cover the classical forms; the app adds custom ones.
public struct PoemPattern: Codable, Identifiable, Equatable {
    public var id: String
    public var name: String
    public var instruction: String
    /// Fixed line count for improvised poems; acrostics always follow the hidden characters.
    public var lines: Int?
    public init(id: String = UUID().uuidString, name: String = "", instruction: String = "", lines: Int? = nil) {
        self.id = id; self.name = name; self.instruction = instruction; self.lines = lines
    }
    public static let builtIn: [PoemPattern] = [
        .init(id: "builtin.jueju", name: "绝句", instruction: "绝句：四句，讲究平仄，偶数句押韵，语言凝练。", lines: 4),
        .init(id: "builtin.lushi", name: "律诗", instruction: "律诗：八句，偶数句押韵，颔联和颈联对仗工整。", lines: 8),
        .init(id: "builtin.couplet", name: "对联", instruction: "对联：上联与下联两句，字数相等，词性相对，平仄相反，上联末字仄声、下联末字平声。", lines: 2),
        .init(id: "builtin.modern", name: "现代诗", instruction: "现代诗：自由分行，不拘平仄，注重意象、节奏和情感。", lines: nil),
    ]
}

/// A word card (词卡): a named set of words or imagery the poem should weave in.
public struct PoemWordCard: Codable, Identifiable, Equatable {
    public var id: UUID
    public var name: String
    public var words: [String]
    public init(id: UUID = UUID(), name: String = "", words: [String] = []) { self.id = id; self.name = name; self.words = words }
}

/// Written only by the containing app; the keyboard reads it.
public struct PoemLibrary: Codable, Equatable {
    public var patterns: [PoemPattern] = []
    public var cards: [PoemWordCard] = []
    public init(patterns: [PoemPattern] = [], cards: [PoemWordCard] = []) { self.patterns = patterns; self.cards = cards }
    public var allPatterns: [PoemPattern] { PoemPattern.builtIn + patterns }
}

/// The keyboard's current choices. Kept in keyboard-private preferences.
public struct PoemOptions: Codable, Equatable {
    public var mode: PoemMode = .improvise
    public var lineLength: PoemLineLength = .seven
    public var patternID: String = "builtin.jueju"
    public var cardIDs: [UUID] = []
    public init() {}
    private enum CodingKeys: String, CodingKey { case mode, lineLength, patternID, cardIDs }
    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        mode = (try? values.decode(PoemMode.self, forKey: .mode)) ?? .improvise
        lineLength = (try? values.decode(PoemLineLength.self, forKey: .lineLength)) ?? .seven
        patternID = (try? values.decode(String.self, forKey: .patternID)) ?? "builtin.jueju"
        cardIDs = (try? values.decode([UUID].self, forKey: .cardIDs)) ?? []
    }
}

public enum PoemError: Error, Equatable { case noHiddenCharacters, tooManyHiddenCharacters(Int) }

public enum PoemPrompt {
    public static let maxHiddenCharacters = 16
    /// Characters an acrostic hides, ignoring spaces and punctuation.
    public static func hiddenCharacters(in source: String) -> [Character] {
        source.filter { !$0.isWhitespace && !$0.isPunctuation && !$0.isSymbol }.map { $0 }
    }
    public static func instruction(source: String, options: PoemOptions, library: PoemLibrary) throws -> String {
        let pattern = library.allPatterns.first { $0.id == options.patternID } ?? PoemPattern.builtIn[0]
        var rules: [String] = []
        switch options.mode {
        case .improvise:
            rules.append("即兴创作：以用户文字的主题、情绪或意象为灵感写一首诗，不必逐字保留原文。")
            rules.append(pattern.lines.map { "全诗共 \($0) 句。" } ?? "句数自定，不超过 12 句。")
        case .acrosticHead, .acrosticTail:
            let hidden = hiddenCharacters(in: source)
            guard !hidden.isEmpty else { throw PoemError.noHiddenCharacters }
            guard hidden.count <= maxHiddenCharacters else { throw PoemError.tooManyHiddenCharacters(maxHiddenCharacters) }
            let place = options.mode == .acrosticHead ? "第一个字" : "最后一个字"
            rules.append("藏\(options.mode == .acrosticHead ? "头" : "尾")：依次用「\(String(hidden))」这 \(hidden.count) 个字作为每一句的\(place)，共 \(hidden.count) 句；这些字不得改动、增删或调换顺序。")
        }
        if options.lineLength != .free { rules.append("每句 \(options.lineLength.rawValue) 个字（不计标点）。") }
        let style = pattern.instruction.trimmingCharacters(in: .whitespacesAndNewlines)
        if !style.isEmpty { rules.append("句式：\(style)") }
        let words = library.cards.filter { options.cardIDs.contains($0.id) }.flatMap(\.words)
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty }
        if !words.isEmpty { rules.append("尽量自然地融入以下词卡中的词语（不必全部使用）：\(words.prefix(40).joined(separator: "、"))。") }
        rules.append("只输出诗的正文：每句单独一行，句末可带标点；不要标题、作者、解释、拼音或引号。")
        return "你是一位擅长中文诗词的诗人。请根据用户消息中的文字创作一首诗。要求：\n"
            + rules.enumerated().map { "\($0.offset + 1). \($0.element)" }.joined(separator: "\n")
    }
}
