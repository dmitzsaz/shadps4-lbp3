// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

import AppKit
import CryptoKit
import Darwin
import Foundation
import UniformTypeIdentifiers

private let requiredBundleName = "shadPS4-lbp3.app"

private struct RuntimeItem {
    let relativePath: String
    let payloadName: String
    let executable: Bool
}

private let runtimeItems = [
    RuntimeItem(relativePath: "Contents/MacOS/shadps4", payloadName: "shadps4", executable: true),
    RuntimeItem(relativePath: "Contents/MacOS/shadps4-core", payloadName: "shadps4-core", executable: true),
    RuntimeItem(relativePath: "Contents/MacOS/partychat", payloadName: "partychat", executable: true),
    RuntimeItem(relativePath: "Contents/MacOS/libvulkan.dylib", payloadName: "libvulkan.dylib", executable: true),
    RuntimeItem(relativePath: "Contents/MacOS/libvulkan_kosmickrisp.dylib",
                payloadName: "libvulkan_kosmickrisp.dylib", executable: true),
    RuntimeItem(relativePath: "Contents/MacOS/kosmickrisp_mesa_icd.json",
                payloadName: "kosmickrisp_mesa_icd.json", executable: false),
    RuntimeItem(relativePath: "Contents/Info.plist", payloadName: "Info.plist", executable: false),
    RuntimeItem(relativePath: "Contents/Resources/LICENSE-shadPS4.txt",
                payloadName: "LICENSE-shadPS4.txt", executable: false),
    RuntimeItem(relativePath: "Contents/Resources/LICENSE-PartyChat.txt",
                payloadName: "LICENSE-PartyChat.txt", executable: false),
]

private enum CoreUpdaterError: LocalizedError {
    case invalidBundleName(String)
    case updaterSelected
    case invalidTarget
    case missingPayload(String)
    case targetIsRunning
    case targetIsNotWritable
    case commandFailed(String, String)
    case atomicReplaceFailed(String)
    case checksumMismatch(String)

    var errorDescription: String? {
        switch self {
        case .invalidBundleName(let name):
            return "Выбран \(name), а нужен именно \(requiredBundleName)."
        case .updaterSelected:
            return "Выбран сам updater. Выберите приложение shadPS4 с игрой."
        case .invalidTarget:
            return "В выбранном приложении нет ожидаемой структуры Contents/MacOS."
        case .missingPayload(let path):
            return "В updater отсутствует runtime-файл \(path). Пересоберите updater."
        case .targetIsRunning:
            return "Сначала полностью закройте выбранный shadPS4-lbp3.app и PartyChat."
        case .targetIsNotWritable:
            return "Нет прав на изменение выбранного приложения. Переместите его в папку, доступную для записи, и повторите попытку."
        case .commandFailed(let command, let output):
            return "Команда \(command) завершилась с ошибкой:\n\(output)"
        case .atomicReplaceFailed(let message):
            return "Не удалось атомарно заменить runtime-файл: \(message)"
        case .checksumMismatch(let path):
            return "Проверка \(path) после копирования не прошла. Старые файлы восстановлены."
        }
    }
}

private struct UpdateResult {
    let targetPath: String
    let manifestHash: String
    let updatedFileCount: Int
    let alreadyCurrent: Bool
}

private struct PreparedRuntimeItem {
    let definition: RuntimeItem
    let payloadURL: URL
    let targetURL: URL
    let stagedURL: URL
    let backupURL: URL
    let targetExisted: Bool
    let payloadHash: String
}

private final class RuntimeUpdater {
    private let fileManager = FileManager.default

    func update(targetURL selectedURL: URL) throws -> UpdateResult {
        let targetURL = selectedURL.standardizedFileURL.resolvingSymlinksInPath()
        let ownBundleURL = Bundle.main.bundleURL.standardizedFileURL.resolvingSymlinksInPath()

        guard targetURL.lastPathComponent == requiredBundleName else {
            throw CoreUpdaterError.invalidBundleName(targetURL.lastPathComponent)
        }
        guard targetURL != ownBundleURL else {
            throw CoreUpdaterError.updaterSelected
        }

        let targetMacOSURL = targetURL.appendingPathComponent("Contents/MacOS", isDirectory: true)
        var targetMacOSIsDirectory: ObjCBool = false
        guard fileManager.fileExists(atPath: targetMacOSURL.path,
                                     isDirectory: &targetMacOSIsDirectory),
              targetMacOSIsDirectory.boolValue else {
            throw CoreUpdaterError.invalidTarget
        }

        guard let payloadRootURL = Bundle.main.resourceURL?
            .appendingPathComponent("Runtime", isDirectory: true) else {
            throw CoreUpdaterError.missingPayload("Runtime")
        }

        if let bundleIdentifier = Bundle(url: targetURL)?.bundleIdentifier,
           !NSRunningApplication.runningApplications(withBundleIdentifier: bundleIdentifier).isEmpty {
            throw CoreUpdaterError.targetIsRunning
        }
        let runtimeURLs = runtimeItems.filter(\.executable).map {
            targetURL.appendingPathComponent($0.relativePath)
        }.filter { fileManager.fileExists(atPath: $0.path) }
        if try anyFileIsInUse(runtimeURLs) {
            throw CoreUpdaterError.targetIsRunning
        }

        guard fileManager.isWritableFile(atPath: targetMacOSURL.path),
              fileManager.isWritableFile(atPath: targetURL.path),
              fileManager.isWritableFile(atPath: targetURL.appendingPathComponent("Contents").path)
        else {
            throw CoreUpdaterError.targetIsNotWritable
        }

        var differences: [(RuntimeItem, URL, URL, String)] = []
        var manifestEntries: [String] = []
        for item in runtimeItems {
            let payloadURL = payloadRootURL.appendingPathComponent(item.payloadName)
            guard fileManager.fileExists(atPath: payloadURL.path) else {
                throw CoreUpdaterError.missingPayload(item.relativePath)
            }

            let targetItemURL = targetURL.appendingPathComponent(item.relativePath)
            let payloadHash = try contentHash(of: payloadURL, item: item)
            manifestEntries.append("\(item.relativePath):\(payloadHash)")
            let targetHash = fileManager.fileExists(atPath: targetItemURL.path)
                ? try contentHash(of: targetItemURL, item: item)
                : nil
            if targetHash != payloadHash {
                differences.append((item, payloadURL, targetItemURL, payloadHash))
            }
        }

        let manifestHash = sha256(of: manifestEntries.sorted().joined(separator: "\n"))
        if differences.isEmpty {
            return UpdateResult(targetPath: targetURL.path, manifestHash: manifestHash,
                                updatedFileCount: 0, alreadyCurrent: true)
        }

        // A failed older updater may have left one of its UUID-named staging files behind.
        // Remove only files whose names exactly match our private staging/rollback format.
        try removeStaleTransactionFiles(from: targetURL)

        let identifier = UUID().uuidString
        let backupDirectory = fileManager.temporaryDirectory
            .appendingPathComponent("shadps4-runtime-backup-\(identifier)", isDirectory: true)

        try fileManager.createDirectory(at: backupDirectory, withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: backupDirectory) }

        var preparedItems: [PreparedRuntimeItem] = []
        defer {
            for item in preparedItems {
                try? fileManager.removeItem(at: item.stagedURL)
            }
        }

        for (item, payloadURL, targetItemURL, payloadHash) in differences {
            if item.executable {
                // Verify the executable while it is still next to its matching payload
                // Info.plist. The target may contain an older Info.plist until commit time.
                try run("/usr/bin/codesign", ["--verify", "--strict", payloadURL.path])
            }

            let targetDirectory = targetItemURL.deletingLastPathComponent()
            try fileManager.createDirectory(at: targetDirectory, withIntermediateDirectories: true)

            let stagedURL = targetDirectory
                .appendingPathComponent(".\(targetItemURL.lastPathComponent).update-\(identifier)")
            let backupURL = backupDirectory.appendingPathComponent(item.relativePath)
            let targetExisted = fileManager.fileExists(atPath: targetItemURL.path)

            if targetExisted {
                try fileManager.createDirectory(at: backupURL.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
                try fileManager.copyItem(at: targetItemURL, to: backupURL)
            }
            try fileManager.copyItem(at: payloadURL, to: stagedURL)
            preparedItems.append(PreparedRuntimeItem(
                definition: item,
                payloadURL: payloadURL,
                targetURL: targetItemURL,
                stagedURL: stagedURL,
                backupURL: backupURL,
                targetExisted: targetExisted,
                payloadHash: payloadHash
            ))
            if item.executable {
                try fileManager.setAttributes([.posixPermissions: 0o755],
                                              ofItemAtPath: stagedURL.path)
            }
            guard try sha256(of: stagedURL) == sha256(of: payloadURL) else {
                throw CoreUpdaterError.checksumMismatch(item.relativePath)
            }
        }

        do {
            for item in preparedItems {
                try atomicReplace(sourceURL: item.stagedURL, destinationURL: item.targetURL)
            }

            for item in preparedItems {
                guard try contentHash(of: item.targetURL, item: item.definition) == item.payloadHash
                else {
                    throw CoreUpdaterError.checksumMismatch(item.definition.relativePath)
                }
            }

            // Refresh only the outer seal after installing already-signed runtime binaries.
            // Resources/Game, Resources/Addons, dry.db and all other bundled data stay untouched.
            try run("/usr/bin/codesign", ["--force", "--sign", "-", targetURL.path])
            try run("/usr/bin/codesign", ["--verify", "--deep", "--strict", targetURL.path])

            return UpdateResult(targetPath: targetURL.path, manifestHash: manifestHash,
                                updatedFileCount: preparedItems.count, alreadyCurrent: false)
        } catch {
            try? restore(items: preparedItems, targetURL: targetURL)
            throw error
        }
    }

    private func restore(items: [PreparedRuntimeItem], targetURL: URL) throws {
        for item in items.reversed() {
            if item.targetExisted {
                let rollbackURL = item.targetURL.deletingLastPathComponent()
                    .appendingPathComponent(".\(item.targetURL.lastPathComponent).rollback-\(UUID().uuidString)")
                defer { try? fileManager.removeItem(at: rollbackURL) }
                try fileManager.copyItem(at: item.backupURL, to: rollbackURL)
                try atomicReplace(sourceURL: rollbackURL, destinationURL: item.targetURL)
            } else if fileManager.fileExists(atPath: item.targetURL.path) {
                try fileManager.removeItem(at: item.targetURL)
            }
        }
        _ = try? run("/usr/bin/codesign", ["--force", "--sign", "-", targetURL.path])
    }

    private func atomicReplace(sourceURL: URL, destinationURL: URL) throws {
        let result = sourceURL.path.withCString { sourcePath in
            destinationURL.path.withCString { destinationPath in
                Darwin.rename(sourcePath, destinationPath)
            }
        }
        guard result == 0 else {
            throw CoreUpdaterError.atomicReplaceFailed(String(cString: strerror(errno)))
        }
    }

    private func sha256(of url: URL) throws -> String {
        let data = try Data(contentsOf: url, options: [.mappedIfSafe])
        return SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    private func contentHash(of url: URL, item: RuntimeItem) throws -> String {
        guard item.executable else { return try sha256(of: url) }

        // Ad-hoc signatures change when the outer app seal is refreshed. Hash an unsigned
        // temporary copy so identical launcher/core/PartyChat/Vulkan code remains idempotent.
        let temporaryDirectory = fileManager.temporaryDirectory
            .appendingPathComponent("shadps4-runtime-hash-\(UUID().uuidString)", isDirectory: true)
        let temporaryURL = temporaryDirectory.appendingPathComponent(url.lastPathComponent)
        try fileManager.createDirectory(at: temporaryDirectory, withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: temporaryDirectory) }
        try fileManager.copyItem(at: url, to: temporaryURL)
        try run("/usr/bin/codesign", ["--remove-signature", temporaryURL.path])
        return try sha256(of: temporaryURL)
    }

    private func sha256(of text: String) -> String {
        SHA256.hash(data: Data(text.utf8)).map { String(format: "%02x", $0) }.joined()
    }

    private func removeStaleTransactionFiles(from targetURL: URL) throws {
        for item in runtimeItems {
            let targetItemURL = targetURL.appendingPathComponent(item.relativePath)
            let targetDirectory = targetItemURL.deletingLastPathComponent()
            guard fileManager.fileExists(atPath: targetDirectory.path) else { continue }

            let candidates = try fileManager.contentsOfDirectory(
                at: targetDirectory,
                includingPropertiesForKeys: nil,
                options: [.skipsSubdirectoryDescendants]
            )
            for candidate in candidates {
                let name = candidate.lastPathComponent
                let prefixes = [
                    ".\(targetItemURL.lastPathComponent).update-",
                    ".\(targetItemURL.lastPathComponent).rollback-",
                ]
                guard prefixes.contains(where: { prefix in
                    name.hasPrefix(prefix) &&
                        UUID(uuidString: String(name.dropFirst(prefix.count))) != nil
                }) else { continue }
                try fileManager.removeItem(at: candidate)
            }
        }
    }

    private func anyFileIsInUse(_ urls: [URL]) throws -> Bool {
        for url in urls {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/sbin/lsof")
            process.arguments = ["-t", "--", url.path]
            let outputPipe = Pipe()
            process.standardOutput = outputPipe
            process.standardError = FileHandle.nullDevice
            try process.run()
            process.waitUntilExit()
            let output = outputPipe.fileHandleForReading.readDataToEndOfFile()
            if process.terminationStatus == 0 && !output.isEmpty {
                return true
            }
        }
        return false
    }

    @discardableResult
    private func run(_ executable: String, _ arguments: [String]) throws -> String {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments

        let outputPipe = Pipe()
        process.standardOutput = outputPipe
        process.standardError = outputPipe
        try process.run()
        process.waitUntilExit()

        let data = outputPipe.fileHandleForReading.readDataToEndOfFile()
        let output = String(decoding: data, as: UTF8.self)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard process.terminationStatus == 0 else {
            throw CoreUpdaterError.commandFailed(URL(fileURLWithPath: executable).lastPathComponent,
                                                 output.isEmpty ? "exit \(process.terminationStatus)" : output)
        }
        return output
    }
}

private final class AppDelegate: NSObject, NSApplicationDelegate {
    private let updater = RuntimeUpdater()
    private var window: NSWindow!
    private var chooseButton: NSButton!
    private var progressIndicator: NSProgressIndicator!
    private var statusLabel: NSTextField!

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildWindow()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    private func buildWindow() {
        let updaterVersion = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString")
            as? String ?? "?"
        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 540, height: 245),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered,
            defer: false
        )
        window.center()
        window.title = "shadPS4 Update \(updaterVersion)"
        window.isReleasedWhenClosed = false

        let titleLabel = NSTextField(labelWithString: "Обновление runtime shadPS4 · \(updaterVersion)")
        titleLabel.font = .systemFont(ofSize: 22, weight: .semibold)
        titleLabel.alignment = .center

        let explanationLabel = NSTextField(wrappingLabelWithString:
            "Выберите shadPS4-lbp3.app, в который уже встроены игра и DLC. " +
            "Updater заменит launcher, core, PartyChat и Vulkan-библиотеки, затем переподпишет приложение. " +
            "Игра, DLC, dry.db и сейвы не изменяются.")
        explanationLabel.alignment = .center
        explanationLabel.textColor = .secondaryLabelColor
        explanationLabel.maximumNumberOfLines = 4

        chooseButton = NSButton(title: "Выбрать .app и обновить", target: self,
                                action: #selector(chooseAndUpdate))
        chooseButton.bezelStyle = .rounded
        chooseButton.controlSize = .large
        chooseButton.keyEquivalent = "\r"

        progressIndicator = NSProgressIndicator()
        progressIndicator.style = .spinning
        progressIndicator.controlSize = .small
        progressIndicator.isDisplayedWhenStopped = false

        statusLabel = NSTextField(labelWithString: "Закройте shadPS4 перед обновлением.")
        statusLabel.alignment = .center
        statusLabel.textColor = .secondaryLabelColor
        statusLabel.lineBreakMode = .byTruncatingMiddle

        let buttonRow = NSStackView(views: [chooseButton, progressIndicator])
        buttonRow.orientation = .horizontal
        buttonRow.alignment = .centerY
        buttonRow.spacing = 12

        let stack = NSStackView(views: [titleLabel, explanationLabel, buttonRow, statusLabel])
        stack.orientation = .vertical
        stack.alignment = .centerX
        stack.spacing = 18
        stack.edgeInsets = NSEdgeInsets(top: 24, left: 30, bottom: 22, right: 30)
        stack.translatesAutoresizingMaskIntoConstraints = false

        guard let contentView = window.contentView else { return }
        contentView.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: contentView.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: contentView.trailingAnchor),
            stack.topAnchor.constraint(equalTo: contentView.topAnchor),
            stack.bottomAnchor.constraint(equalTo: contentView.bottomAnchor),
            explanationLabel.widthAnchor.constraint(equalToConstant: 470),
            statusLabel.widthAnchor.constraint(equalToConstant: 470),
        ])
    }

    @objc private func chooseAndUpdate() {
        let panel = NSOpenPanel()
        panel.title = "Выберите shadPS4-lbp3.app с игрой"
        panel.prompt = "Обновить core"
        panel.message = "Игра, DLC, dry.db и сейвы останутся без изменений"
        panel.allowedContentTypes = [.applicationBundle]
        panel.allowsMultipleSelection = false
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.directoryURL = fileManagerDesktopURL()

        panel.beginSheetModal(for: window) { [weak self] response in
            guard response == .OK, let targetURL = panel.url else { return }
            self?.performUpdate(targetURL: targetURL)
        }
    }

    private func performUpdate(targetURL: URL) {
        chooseButton.isEnabled = false
        progressIndicator.startAnimation(nil)
        statusLabel.stringValue = "Обновляю и переподписываю…"

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            do {
                guard let self else { return }
                let result = try self.updater.update(targetURL: targetURL)
                DispatchQueue.main.async {
                    self.finish(result: result)
                }
            } catch {
                DispatchQueue.main.async {
                    self?.finish(error: error)
                }
            }
        }
    }

    private func finish(result: UpdateResult) {
        chooseButton.isEnabled = true
        progressIndicator.stopAnimation(nil)
        statusLabel.stringValue = result.targetPath

        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.messageText = result.alreadyCurrent ? "Runtime уже актуален" : "Runtime успешно обновлён"
        alert.informativeText = result.alreadyCurrent
            ? "В выбранном приложении уже находятся все версии файлов из updater."
            : "Обновлено файлов: \(result.updatedFileCount). Checksum проверены, приложение переподписано.\n\n" +
              "Manifest SHA-256: \(String(result.manifestHash.prefix(16)))…"
        alert.addButton(withTitle: "Готово")
        alert.beginSheetModal(for: window)
    }

    private func finish(error: Error) {
        chooseButton.isEnabled = true
        progressIndicator.stopAnimation(nil)
        statusLabel.stringValue = "Обновление не выполнено"

        let updaterVersion = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString")
            as? String ?? "?"
        let alert = NSAlert()
        alert.alertStyle = .critical
        alert.messageText = "Обновление не выполнено"
        alert.informativeText = "Updater \(updaterVersion)\n\n\(error.localizedDescription)"
        alert.addButton(withTitle: "OK")
        alert.beginSheetModal(for: window)
    }

    private func fileManagerDesktopURL() -> URL {
        FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask).first
            ?? FileManager.default.homeDirectoryForCurrentUser
    }
}

private func runCommandLineUpdateIfRequested() -> Bool {
    let arguments = CommandLine.arguments
    guard arguments.count == 3, arguments[1] == "--target" else { return false }

    do {
        let result = try RuntimeUpdater().update(targetURL: URL(fileURLWithPath: arguments[2]))
        print(result.alreadyCurrent ? "Runtime is already current: \(result.targetPath)"
                                    : "Runtime updated (\(result.updatedFileCount) files): \(result.targetPath)")
        exit(EXIT_SUCCESS)
    } catch {
        fputs("Runtime update failed: \(error.localizedDescription)\n", stderr)
        exit(EXIT_FAILURE)
    }
}

if !runCommandLineUpdateIfRequested() {
    let application = NSApplication.shared
    let delegate = AppDelegate()
    application.delegate = delegate
    application.setActivationPolicy(.regular)
    application.run()
}
