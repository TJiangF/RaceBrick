import SwiftUI

struct AuthView: View {
    @EnvironmentObject private var model: AppModel
    @State private var username = ""
    @State private var password = ""
    @State private var registering = false
    @State private var loading = false

    var body: some View {
        ZStack {
            LinearGradient(colors: [Color(red: 0.03, green: 0.06, blue: 0.09),
                                    Color(red: 0.08, green: 0.13, blue: 0.18)],
                           startPoint: .topLeading, endPoint: .bottomTrailing)
                .ignoresSafeArea()
            VStack(spacing: 18) {
                Spacer()
                Text("RaceBrick")
                    .font(.system(size: 40, weight: .black, design: .rounded))
                    .foregroundStyle(.white)
                Text("RACE CLOUD")
                    .font(.system(size: 13, weight: .bold, design: .monospaced))
                    .tracking(4)
                    .foregroundStyle(.cyan)
                VStack(spacing: 12) {
                    TextField("用户名", text: $username)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .textContentType(.username)
                        .textFieldStyle(.roundedBorder)
                    SecureField("密码（至少 8 位）", text: $password)
                        .textContentType(registering ? .newPassword : .password)
                        .textFieldStyle(.roundedBorder)
                    Button {
                        Task { await submit() }
                    } label: {
                        if loading { ProgressView().tint(.black) }
                        else { Text(registering ? "注册并登录" : "登录").fontWeight(.bold) }
                    }
                    .frame(maxWidth: .infinity)
                    .buttonStyle(.borderedProminent)
                    .tint(.cyan)
                    .disabled(username.isEmpty || password.count < 8 || loading)
                    Button(registering ? "已有账户，返回登录" : "创建账户") {
                        registering.toggle()
                    }
                    .foregroundStyle(.cyan)
                }
                .padding(20)
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 20))
                .padding(.horizontal, 28)
                Spacer()
                Text("racebrick.tfworks.cn")
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
                    .padding(.bottom, 20)
            }
        }
    }

    private func submit() async {
        loading = true
        if registering { await model.register(username: username, password: password) }
        else { await model.login(username: username, password: password) }
        loading = false
    }
}
