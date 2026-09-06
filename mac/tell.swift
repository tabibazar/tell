// tell -- write a line of text to the ESP32-Screen over BLE.
//
//   tell "some text"
//   echo "some text" | tell
//   tell --device big "some text"
//   tell --list
//
// Build: swiftc -O mac/tell.swift -o mac/tell

import Foundation
import CoreBluetooth

let serviceUUID = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
let rxUUID      = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
let timeUUID    = CBUUID(string: "6E400004-B5A3-F393-E0A9-E50E24DCCA9E")
let maxMessage  = 512
let timeout     = 10.0

func note(_ msg: String) {
    FileHandle.standardError.write(Data("tell: \(msg)\n".utf8))
}

func fail(_ msg: String) -> Never {
    note(msg)
    exit(1)
}

/// Seconds since local midnight. Sending this rather than a Unix timestamp
/// means the firmware never has to know about timezones.
func secondsSinceLocalMidnight() -> UInt32 {
    let now = Date()
    let midnight = Calendar.current.startOfDay(for: now)
    return UInt32(now.timeIntervalSince(midnight))
}

/// Parses the leading flags, leaving the message words.
func parseArgs() -> (syncOnly: Bool, listOnly: Bool, device: String?, words: [String]) {
    var args = Array(CommandLine.arguments.dropFirst())
    var syncOnly = false
    var listOnly = false
    var device: String? = nil

    while let first = args.first {
        if first == "--list" {
            listOnly = true
            args.removeFirst()
        } else if first == "--sync" {
            syncOnly = true
            args.removeFirst()
        } else if first == "--device" {
            args.removeFirst()
            guard let name = args.first else { fail("--device needs a name") }
            device = name
            args.removeFirst()
        } else if first.hasPrefix("--device=") {
            device = String(first.dropFirst("--device=".count))
            args.removeFirst()
        } else {
            break
        }
    }
    return (syncOnly, listOnly, device, args)
}

let opts = parseArgs()
let syncOnly = opts.syncOnly
let listOnly = opts.listOnly
let wantedDevice = opts.device
let listSeconds = 4.0

func readMessage() -> Data {
    let args = opts.words
    var text: String
    if syncOnly || listOnly { return Data() }
    if args.isEmpty {
        let raw = FileHandle.standardInput.readDataToEndOfFile()
        text = String(data: raw, encoding: .utf8) ?? ""
    } else {
        text = args.joined(separator: " ")
    }
    while text.hasSuffix("\n") { text.removeLast() }

    var bytes = Data(text.utf8)
    if bytes.count > maxMessage {
        note("warning: message truncated to \(maxMessage) bytes")
        bytes = bytes.prefix(maxMessage)
    }
    return bytes
}

final class Client: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private let payload: Data
    private var outstanding = 0
    /// Names seen while listing, mapped to their strongest signal.
    private var seen: [String: Int] = [:]

    init(payload: Data) {
        self.payload = payload
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        switch c.state {
        case .poweredOn:
            // Scan by service UUID: CoreBluetooth caches peripheral names, so
            // matching on name can miss a renamed device.
            c.scanForPeripherals(withServices: [serviceUUID])
            if listOnly {
                DispatchQueue.main.asyncAfter(deadline: .now() + listSeconds) {
                    self.report()
                    exit(0)
                }
            }
        case .poweredOff:
            fail("Bluetooth is off")
        case .unauthorized:
            fail("Bluetooth permission denied -- grant it to your terminal in "
                 + "System Settings > Privacy & Security > Bluetooth")
        case .unsupported:
            fail("Bluetooth LE unsupported on this machine")
        default:
            break
        }
    }

    /// Prints what the scan found, newest signal strength first.
    func report() {
        if seen.isEmpty {
            note("no screens found within \(Int(listSeconds))s")
            exit(1)
        }
        for (name, rssi) in seen.sorted(by: { $0.value > $1.value }) {
            print("\(name)  (signal \(rssi) dBm)")
        }
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        if listOnly {
            // Only the scan response carries a trustworthy name.
            if let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String {
                let rssi = RSSI.intValue
                if rssi > (seen[name] ?? Int.min) { seen[name] = rssi }
            }
            return
        }
        // With several boards in range, match the advertised name. It must come
        // from the scan response: p.name is cached by CoreBluetooth across
        // sessions, so a renamed board keeps answering to its old name. A
        // callback without the scan response is skipped, not guessed at --
        // scanning continues and a later one carries the name.
        if let wanted = wantedDevice {
            guard let advertised = advertisementData[CBAdvertisementDataLocalNameKey] as? String,
                  advertised.caseInsensitiveCompare(wanted) == .orderedSame else { return }
        }
        c.stopScan()
        peripheral = p
        p.delegate = self
        c.connect(p)
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral,
                        error: Error?) {
        fail("connect failed: \(error?.localizedDescription ?? "unknown")")
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        p.discoverServices([serviceUUID])
    }

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = p.services?.first(where: { $0.uuid == serviceUUID }) else {
            fail("Nordic UART service not found on device")
        }
        p.discoverCharacteristics([rxUUID, timeUUID], for: svc)
    }

    func peripheral(_ p: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        guard let chr = service.characteristics?.first(where: { $0.uuid == rxUUID }) else {
            fail("RX characteristic not found")
        }

        // Re-sync the clock on every connect. The board has no RTC, so this is
        // what keeps it accurate; a missing characteristic is not fatal, since
        // older firmware still displays text fine.
        var writes: [(Data, CBCharacteristic)] = []
        if let timeChr = service.characteristics?.first(where: { $0.uuid == timeUUID }) {
            var secs = secondsSinceLocalMidnight().littleEndian
            let stamp = Data(bytes: &secs, count: 4)
            writes.append((stamp, timeChr))
        } else if syncOnly {
            fail("clock characteristic not found -- is the firmware up to date?")
        }

        if !syncOnly {
            if payload.isEmpty {
                // An empty payload is a deliberate "clear", which the firmware
                // treats as "return to the clock".
                writes.append((Data(), chr))
            } else {
                let limit = max(20, p.maximumWriteValueLength(for: .withResponse))
                var offset = 0
                while offset < payload.count {
                    let end = min(offset + limit, payload.count)
                    writes.append((payload.subdata(in: offset..<end), chr))
                    offset = end
                }
            }
        }

        outstanding = writes.count
        for (data, target) in writes {
            p.writeValue(data, for: target, type: .withResponse)
        }
        return

    }

    func peripheral(_ p: CBPeripheral, didWriteValueFor chr: CBCharacteristic,
                    error: Error?) {
        if let error = error { fail("write failed: \(error.localizedDescription)") }
        outstanding -= 1
        if outstanding <= 0 {
            // The firmware completes a message after 50 ms of quiet.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { exit(0) }
        }
    }
}

let client = Client(payload: readMessage())

DispatchQueue.main.asyncAfter(deadline: .now() + timeout) {
    if let wanted = wantedDevice {
        fail("no device named \(wanted) found within \(Int(timeout))s")
    }
    fail("no screen found within \(Int(timeout))s")
}
dispatchMain()
