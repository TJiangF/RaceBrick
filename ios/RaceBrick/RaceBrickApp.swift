import SwiftUI
import RaceBrickCore

@main
struct RaceBrickApp: App {
    @StateObject private var model = AppModel()
    @StateObject private var ble = BLEManager()

    var body: some Scene {
        WindowGroup {
            Group {
                if model.authenticated {
                    MainTabView()
                } else {
                    AuthView()
                }
            }
            .environmentObject(model)
            .environmentObject(ble)
            .task { await model.restore() }
            .alert("错误", isPresented: Binding(
                get: { model.errorMessage != nil },
                set: { if !$0 { model.errorMessage = nil } }
            )) {
                Button("确定", role: .cancel) { model.errorMessage = nil }
            } message: {
                Text(model.errorMessage ?? "")
            }
        }
    }
}

struct MainTabView: View {
    var body: some View {
        TabView {
            DevicesView()
                .tabItem { Label("Race", systemImage: "flag.checkered") }
            BLEView()
                .tabItem { Label("Link", systemImage: "antenna.radiowaves.left.and.right") }
            MediaView()
                .tabItem { Label("Media", systemImage: "music.note") }
        }
        .tint(.cyan)
    }
}
