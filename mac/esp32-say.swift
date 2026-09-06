// esp32-say -- write a line of text to the ESP32-Screen over BLE.
//
//   esp32-say "some text"
//   echo "some text" | esp32-say
//
// Build: swiftc -O mac/esp32-say.swift -o mac/esp32-say

import Foundation
import CoreBluetooth

let serviceUUID = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
let rxUUID      = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
let maxMessage  = 512
let timeout     = 10.0

func note(_ msg: String) {
    FileHandle.standardError.write(Data("esp32-say: \(msg)\n".utf8))
}

func fail(_ msg: String) -> Never {
    note(msg)
    exit(1)
}

func readMessage() -> Data {
    let args = Array(CommandLine.arguments.dropFirst())
    var text: String
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

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
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
        p.discoverCharacteristics([rxUUID], for: svc)
    }

    func peripheral(_ p: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        guard let chr = service.characteristics?.first(where: { $0.uuid == rxUUID }) else {
            fail("RX characteristic not found")
        }

        // An empty payload is a deliberate "clear the screen".
        if payload.isEmpty {
            outstanding = 1
            p.writeValue(Data(), for: chr, type: .withResponse)
            return
        }

        let limit = max(20, p.maximumWriteValueLength(for: .withResponse))
        var offset = 0
        var chunks: [Data] = []
        while offset < payload.count {
            let end = min(offset + limit, payload.count)
            chunks.append(payload.subdata(in: offset..<end))
            offset = end
        }
        outstanding = chunks.count
        for chunk in chunks {
            p.writeValue(chunk, for: chr, type: .withResponse)
        }
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
    fail("no ESP32-Screen found within \(Int(timeout))s")
}
dispatchMain()
