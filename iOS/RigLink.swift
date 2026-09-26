import Foundation

/// Transport only. Pending motion is a single replaceable target, never a backlog.
@MainActor
final class RigLink {
  var onOpen: (() -> Void)?
  var onLine: ((String) -> Void)?
  var onDisconnect: ((String?) -> Void)?
  private var socket: URLSessionWebSocketTask?
  private var connectionTask: Task<Void, Never>?
  private var writer: Task<Void, Never>?
  private var pendingMove: String?
  private var commands: [String] = []
  private var generation = UUID()
  private var ready = false

  func connect(to url: URL) {
    disconnect()
    let id = generation
    connectionTask = Task { [weak self] in
      while !Task.isCancelled {
        guard let self, self.generation == id else { return }
        let socket = URLSession.shared.webSocketTask(with: url)
        self.socket = socket
        socket.resume()
        do {
          try await socket.send(.string("hello"))
          guard self.generation == id, !Task.isCancelled else { return }
          self.ready = true
          self.onOpen?()
          while !Task.isCancelled {
            let message = try await socket.receive()
            guard self.generation == id else { return }
            if case .string(let line) = message { self.onLine?(line) }
          }
        } catch {
          guard self.generation == id, !Task.isCancelled else { return }
          self.onDisconnect?(error.localizedDescription)
        }
        self.ready = false
        self.writer?.cancel()
        self.writer = nil
        self.commands.removeAll()
        self.pendingMove = nil
        socket.cancel(with: .goingAway, reason: nil)
        do { try await Task.sleep(for: .seconds(1)) } catch { return }
      }
    }
  }

  func disconnect() {
    generation = UUID()
    connectionTask?.cancel()
    writer?.cancel()
    socket?.cancel(with: .goingAway, reason: nil)
    socket = nil
    writer = nil
    ready = false
    commands.removeAll()
    pendingMove = nil
  }

  func discardMotion() { pendingMove = nil }

  func move(_ line: String) {
    guard ready else { return }
    pendingMove = line
    drain()
  }

  func command(_ line: String) {
    guard ready else { return }
    if line == "stop" || line == "center" { pendingMove = nil }
    if !commands.contains(line) { commands.append(line) }
    drain()
  }

  private func drain() {
    guard writer == nil, let socket else { return }
    let id = generation
    writer = Task { [weak self] in
      guard let self else { return }
      defer { if self.generation == id { self.writer = nil } }
      while self.ready, self.generation == id, !Task.isCancelled {
        let line: String
        if !self.commands.isEmpty { line = self.commands.removeFirst() }
        else if let move = self.pendingMove { line = move; self.pendingMove = nil }
        else { return }
        do { try await socket.send(.string(line)) }
        catch {
          guard self.generation == id else { return }
          self.ready = false
          socket.cancel(with: .goingAway, reason: nil)
          return
        }
      }
    }
  }
}
