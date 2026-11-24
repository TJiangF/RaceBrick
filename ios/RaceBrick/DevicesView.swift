import SwiftUI
import RaceBrickCore

struct DevicesView: View {
    @EnvironmentObject private var model: AppModel
    @State private var showingBind = false
    @State private var showingAdmin = false
    @State private var uid = ""

    var body: some View {
        NavigationStack {
            List {
                if model.devices.isEmpty {
                    VStack(spacing: 10) {
                        Image(systemName: "antenna.radiowaves.left.and.right.slash")
                            .font(.largeTitle).foregroundStyle(.secondary)
                        Text("没有绑定设备").font(.headline)
                        Text("在 ESP 上查看唯一码，然后绑定到当前账户。")
                            .font(.footnote).foregroundStyle(.secondary)
                        Button("绑定设备") { showingBind = true }
                    }
                    .frame(maxWidth: .infinity).padding(.vertical, 30)
                } else {
                    Section("我的设备") {
                        ForEach(model.devices) { device in
                            NavigationLink(value: device.uid) {
                                DeviceRow(device: device)
                            }
                        }
                    }
                }
                Section {
                    Button {
                        showingBind = true
                    } label: {
                        Label("绑定 ESP 唯一码", systemImage: "link.badge.plus")
                    }
                }
                if model.isAdmin {
                    Section("管理员") {
                        Button { showingAdmin = true } label: {
                            Label("管理后台", systemImage: "person.2.badge.gearshape")
                        }
                    }
                }
            }
            .navigationTitle("RaceBrick Cloud")
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Text(model.user?.username ?? "")
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Button("刷新") { Task { await model.loadDevices() } }
                        if let me = model.user { Button(me.username + " · 退出", role: .destructive) { model.logout() } }
                    } label: { Image(systemName: "person.crop.circle") }
                }
            }
            .refreshable { await model.loadDevices() }
            .navigationDestination(for: String.self) { uid in
                DeviceDetailView(uid: uid)
            }
            .sheet(isPresented: $showingBind) {
                BindDeviceSheet(uid: $uid)
                    .environmentObject(model)
            }
            .sheet(isPresented: $showingAdmin) {
                AdminUsersView().environmentObject(model)
            }
            .task { await model.loadDevices() }
        }
    }
}

private struct DeviceRow: View {
    let device: CloudDevice
    var body: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(device.online ? .green : .gray.opacity(0.4))
                .frame(width: 9, height: 9)
            VStack(alignment: .leading, spacing: 3) {
                Text(device.displayName).font(.headline)
                Text(device.uid).font(.caption.monospaced()).foregroundStyle(.secondary)
                Text("\(device.tracks ?? 0) tracks · \(device.sessions ?? 0) race logs")
                    .font(.caption2).foregroundStyle(.secondary)
            }
            Spacer()
            if let at = device.lastSyncAt {
                Text(Self.date.string(from: Date(timeIntervalSince1970: TimeInterval(at))))
                    .font(.caption2.monospaced()).foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 3)
    }
    private static let date: DateFormatter = {
        let f = DateFormatter(); f.dateFormat = "MM-dd HH:mm"; return f
    }()
}

private struct BindDeviceSheet: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @Binding var uid: String

    var body: some View {
        NavigationStack {
            Form {
                Section("ESP 唯一码") {
                    TextField("esp32s3-XXXXXX", text: $uid)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .font(.body.monospaced())
                }
                Section {
                    Text("绑定后，设备同步的 Track 和 Race Log 会自动显示在此账户。")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            .navigationTitle("绑定设备")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("取消") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("绑定") {
                        Task {
                            await model.bind(uid: uid.trimmingCharacters(in: .whitespaces))
                            dismiss()
                        }
                    }
                    .disabled(uid.trimmingCharacters(in: .whitespaces).count < 3)
                }
            }
        }
    }
}

struct AdminUsersView: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        NavigationStack {
            List(model.adminUsers) { u in
                VStack(alignment: .leading) {
                    Text(u.username).font(.headline)
                    Text("\(u.role) · \(u.devices) devices").font(.caption).foregroundStyle(.secondary)
                }
            }
            .overlay { if model.adminUsers.isEmpty { ProgressView() } }
            .navigationTitle("账户")
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("关闭") { dismiss() } } }
            .task { await model.loadAdminUsers() }
        }
    }
}
