import Foundation

enum RigProtocol {
  struct Limits {
    var tilt: ClosedRange<Double> = -85...85
    var pan: ClosedRange<Double> = -85...85
  }

  struct Position {
    var tilt: Double
    var pan: Double
    var moving: Bool
  }

  enum Message {
    case hello(version: Int?, tilt: ClosedRange<Double>?, pan: ClosedRange<Double>?)
    case position(Position)
    case heartbeat(reset: String?)
    case error(String)
    case bridge(connected: Bool)
    case unknown
  }

  static func clamp(_ value: Double, to range: ClosedRange<Double>) -> Double {
    min(range.upperBound, max(range.lowerBound, value.isFinite ? value : 0))
  }

  static func number(_ value: Double) -> String {
    String(format: "%.1f", locale: Locale(identifier: "en_US_POSIX"), abs(value) < 0.05 ? 0 : value)
  }

  static func move(tilt: Double, pan: Double, limits: Limits) -> String {
    "move \(number(clamp(tilt, to: limits.tilt))) \(number(clamp(pan, to: limits.pan)))"
  }

  static func parse(_ line: String) -> Message {
    let words = line.split(separator: " ")
    guard let type = words.first else { return .unknown }
    var fields: [String: String] = [:]
    for word in words.dropFirst() {
      let pair = word.split(separator: "=", maxSplits: 1)
      if pair.count == 2 { fields[String(pair[0])] = String(pair[1]) }
    }
    switch type {
    case "hello":
      return .hello(version: fields["proto"].flatMap(Int.init),
                    tilt: range(fields["tilt"]), pan: range(fields["pan"]))
    case "pos":
      guard let tilt = finite(fields["tilt"]), let pan = finite(fields["pan"]),
            let moving = fields["moving"], moving == "0" || moving == "1" else { return .unknown }
      return .position(Position(tilt: tilt, pan: pan, moving: moving == "1"))
    case "hb": return .heartbeat(reset: fields["reset"])
    case "err": return .error(String(line.dropFirst(3)).trimmingCharacters(in: .whitespaces))
    case "bridge":
      guard let serial = fields["serial"], serial == "connected" || serial == "disconnected" else { return .unknown }
      return .bridge(connected: serial == "connected")
    default: return .unknown
    }
  }

  private static func finite(_ text: String?) -> Double? {
    guard let text, let value = Double(text), value.isFinite else { return nil }
    return value
  }

  private static func range(_ text: String?) -> ClosedRange<Double>? {
    guard let parts = text?.components(separatedBy: ".."), parts.count == 2,
          let low = finite(parts[0]), let high = finite(parts[1]), low < high,
          low <= 0, high >= 0 else { return nil }
    return low...high
  }
}
