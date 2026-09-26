import Foundation
import Observation

@MainActor @Observable
final class RigController {
  enum InputSource { case slider }
  enum LinkState { case disconnected, connecting, bridgeOnly, rigSilent, connected }

  var tilt: Double {
    get { targetTilt }
    set { targetTilt = RigProtocol.clamp(newValue, to: limits.tilt); motionChanged() }
  }
  var pan: Double {
    get { targetPan }
    set { targetPan = RigProtocol.clamp(newValue, to: limits.pan); motionChanged() }
  }
  var tiltSource: InputSource = .slider
  var panSource: InputSource = .slider
  private(set) var link: LinkState = .disconnected
  private(set) var reported: RigProtocol.Position?
  private(set) var limits = RigProtocol.Limits()
  private(set) var lastLine = ""
  private(set) var lastError = ""
  private(set) var lastReset = ""
  private(set) var protocolWarning: String?
  var host = UserDefaults.standard.string(forKey: "rigHost") ?? "127.0.0.1"
  var port = UserDefaults.standard.string(forKey: "rigPort") ?? "8765"
  private var targetTilt = 0.0
  private var targetPan = 0.0
  @ObservationIgnored private let transport = RigLink()
  @ObservationIgnored private var clockTask: Task<Void, Never>?
  @ObservationIgnored private var socketUp = false
  @ObservationIgnored private var serialUp = false
  @ObservationIgnored private var receivedHello = false
  @ObservationIgnored private var helloRequested = false
  @ObservationIgnored private var lastRigLine = ContinuousClock.now
  @ObservationIgnored private var lastMove: String?
  @ObservationIgnored private var dirty = false
  @ObservationIgnored private var stopped = false

  var statusText: String {
    switch link {
    case .connected: "Connected"
    case .bridgeOnly: "Rig unplugged"
    case .rigSilent: "Rig not responding"
    case .disconnected, .connecting: "Disconnected"
    }
  }

  func start() {
    guard clockTask == nil else { return }
    transport.onOpen = { [weak self] in
      guard let self else { return }
      self.socketUp = true
      self.helloRequested = true
      self.lastRigLine = .now
      self.link = .rigSilent
    }
    transport.onLine = { [weak self] in self?.receive($0) }
    transport.onDisconnect = { [weak self] error in
      guard let self else { return }
      self.socketUp = false
      self.serialUp = false
      self.receivedHello = false
      self.helloRequested = false
      self.link = .disconnected
      self.lastMove = nil
      if let error { self.lastError = error }
    }
    reconnect()
    clockTask = Task { [weak self] in
      while !Task.isCancelled {
        do { try await Task.sleep(for: .milliseconds(34)) } catch { return }
        guard let self else { return }
        self.flush()
        if self.socketUp && self.serialUp && self.lastRigLine.duration(to: .now) >= .seconds(3) {
          self.link = .rigSilent
        }
      }
    }
  }

  func shutdown() {
    clockTask?.cancel()
    clockTask = nil
    transport.disconnect()
    socketUp = false
    link = .disconnected
  }

  func reconnect() {
    host = host.trimmingCharacters(in: .whitespacesAndNewlines)
    guard let portNumber = Int(port), (1...65535).contains(portNumber),
          !host.isEmpty, !host.contains("/"), !host.contains(" ") else {
      lastError = "Enter a host name or IP address and a port from 1 to 65535."
      return
    }
    var components = URLComponents()
    components.scheme = "ws"
    components.host = host
    components.port = portNumber
    guard let url = components.url else { lastError = "Invalid bridge address."; return }
    UserDefaults.standard.set(host, forKey: "rigHost")
    UserDefaults.standard.set(port, forKey: "rigPort")
    socketUp = false
    serialUp = false
    receivedHello = false
    helloRequested = false
    lastMove = nil
    reported = nil
    protocolWarning = nil
    link = .connecting
    transport.connect(to: url)
  }

  func editingChanged(_ editing: Bool) {
    if !editing && !stopped { flush(force: true) }
  }

  func center() {
    stopped = false
    targetTilt = 0
    targetPan = 0
    dirty = false
    lastMove = currentMove
    if canSend { transport.command("center") }
  }

  func stop() {
    stopped = true
    dirty = false
    transport.discardMotion()
    if canSend { transport.command("stop") }
  }

  private var canSend: Bool { socketUp && serialUp }
  private var currentMove: String { RigProtocol.move(tilt: tilt, pan: pan, limits: limits) }

  private func motionChanged() {
    stopped = false
    dirty = true
  }

  private func flush(force: Bool = false) {
    guard canSend, !stopped, dirty || force else { return }
    let line = currentMove
    dirty = false
    if force || line != lastMove {
      transport.move(line)
      lastMove = line
    }
  }

  private func synchronize() {
    if stopped { transport.command("stop") }
    else { flush(force: true) }
  }

  private func receive(_ line: String) {
    lastLine = line
    let message = RigProtocol.parse(line)
    switch message {
    case .bridge(let connected):
      serialUp = connected
      if connected {
        lastRigLine = .now
        link = .rigSilent
        if !receivedHello && !helloRequested {
          transport.command("hello")
          helloRequested = true
        }
        synchronize()
      } else {
        link = .bridgeOnly
        receivedHello = false
        helloRequested = false
        reported = nil
        transport.discardMotion()
      }
      return
    case .unknown: return
    default:
      serialUp = true
      lastRigLine = .now
      link = .connected
    }
    switch message {
    case .hello(let version, let tiltRange, let panRange):
      receivedHello = true
      helloRequested = false
      protocolWarning = version == 1 ? nil : "Protocol mismatch: expected version 1."
      limits = RigProtocol.Limits(tilt: tiltRange ?? -85...85, pan: panRange ?? -85...85)
      targetTilt = RigProtocol.clamp(targetTilt, to: limits.tilt)
      targetPan = RigProtocol.clamp(targetPan, to: limits.pan)
      synchronize()
    case .position(let position):
      reported = position
      if stopped {
        targetTilt = RigProtocol.clamp(position.tilt, to: limits.tilt)
        targetPan = RigProtocol.clamp(position.pan, to: limits.pan)
      }
    case .heartbeat(let reset): if let reset { lastReset = reset }
    case .error(let reason): lastError = reason
    default: break
    }
  }
}
