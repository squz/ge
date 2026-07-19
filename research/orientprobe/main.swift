// OrientProbe — minimal orientation/letterbox probe for the ge orientation report.
// One binary; behavior selected by env vars set via SIMCTL_CHILD_*:
//   PROBE_SUPPORTED = "land" | "port" | "all"   (runtime supportedInterfaceOrientations mask; default all)
//   PROBE_FREEZE    = "1"                        (return true from prefersInterfaceOrientationLocked — TN3192)
//   PROBE_FREEZE_DELAY = seconds                 (arm freeze after N seconds instead of immediately)
// Paints: green body, red bar = scene TOP edge, blue bar = scene LEADING edge,
// yellow 40pt squares in all four corners (full-bleed check), and a live metrics label.
// Logs one METRICS line per second to stdout.

import UIKit

func env(_ k: String) -> String? { ProcessInfo.processInfo.environment[k] }

var freezeArmed = env("PROBE_FREEZE") == "1" && env("PROBE_FREEZE_DELAY") == nil

final class ProbeVC: UIViewController {
    let label = UILabel()
    var timer: Timer?
    var tick = 0

    override var supportedInterfaceOrientations: UIInterfaceOrientationMask {
        switch env("PROBE_SUPPORTED") {
        case "land": return .landscape
        case "port": return [.portrait, .portraitUpsideDown]
        default: return .all
        }
    }

#if PROBE26
    override var prefersInterfaceOrientationLocked: Bool { freezeArmed }
#endif

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemGreen
        let top = UIView(); top.backgroundColor = .systemRed
        let lead = UIView(); lead.backgroundColor = .systemBlue
        for v in [top, lead] { v.translatesAutoresizingMaskIntoConstraints = false; view.addSubview(v) }
        NSLayoutConstraint.activate([
            top.topAnchor.constraint(equalTo: view.topAnchor),
            top.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            top.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            top.heightAnchor.constraint(equalToConstant: 40),
            lead.topAnchor.constraint(equalTo: view.topAnchor),
            lead.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            lead.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            lead.widthAnchor.constraint(equalToConstant: 40),
        ])
        for (h, v) in [(view.topAnchor, view.leadingAnchor), (view.topAnchor, view.trailingAnchor),
                       (view.bottomAnchor, view.leadingAnchor), (view.bottomAnchor, view.trailingAnchor)] {
            let c = UIView(); c.backgroundColor = .systemYellow
            c.translatesAutoresizingMaskIntoConstraints = false
            view.addSubview(c)
            NSLayoutConstraint.activate([
                c.widthAnchor.constraint(equalToConstant: 40), c.heightAnchor.constraint(equalToConstant: 40),
                (h == view.topAnchor) ? c.topAnchor.constraint(equalTo: h) : c.bottomAnchor.constraint(equalTo: h),
                (v == view.leadingAnchor) ? c.leadingAnchor.constraint(equalTo: v) : c.trailingAnchor.constraint(equalTo: v),
            ])
        }
        label.numberOfLines = 0
        label.font = .monospacedSystemFont(ofSize: 14, weight: .bold)
        label.textColor = .black
        label.backgroundColor = UIColor.white.withAlphaComponent(0.85)
        label.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(label)
        NSLayoutConstraint.activate([
            label.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            label.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            label.widthAnchor.constraint(lessThanOrEqualTo: view.widthAnchor, multiplier: 0.9),
        ])
        if let d = env("PROBE_FREEZE_DELAY").flatMap(Double.init), env("PROBE_FREEZE") == "1" {
            DispatchQueue.main.asyncAfter(deadline: .now() + d) { [weak self] in
                freezeArmed = true
#if PROBE26
                if #available(iOS 26.0, *) {
                    self?.setNeedsUpdateOfPrefersInterfaceOrientationLocked()
                }
#endif
                print("PROBE: freeze armed after \(d)s")
            }
        }
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in self?.report() }
        report()
    }

    func report() {
        tick += 1
        let scr = UIScreen.main.bounds
        let scrScale = UIScreen.main.scale
        let win = view.window?.bounds ?? .zero
        let scene = view.window?.windowScene
        let io = scene?.interfaceOrientation.rawValue ?? -1
        var geo = "n/a"
        if #available(iOS 16.0, *), let s = scene {
            let g = s.effectiveGeometry
            geo = "ori=\(g.interfaceOrientation.rawValue)"
            if #available(iOS 26.0, *) { geo += " locked=\(g.isInterfaceOrientationLocked)" }
        }
        let dev = UIDevice.current.orientation.rawValue
        let safe = view.safeAreaInsets
        let txt = """
        tick=\(tick) sup=\(env("PROBE_SUPPORTED") ?? "all") freeze=\(freezeArmed)
        screen=\(Int(scr.width))x\(Int(scr.height))@\(scrScale)
        window=\(Int(win.width))x\(Int(win.height))
        ifaceOri=\(io) devOri=\(dev) \(geo)
        safe=(t\(Int(safe.top)),l\(Int(safe.left)),b\(Int(safe.bottom)),r\(Int(safe.right)))
        """
        label.text = txt
        print("METRICS " + txt.replacingOccurrences(of: "\n", with: " | "))
    }
}

final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?
    func application(_ a: UIApplication, didFinishLaunchingWithOptions o: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        window = UIWindow(frame: UIScreen.main.bounds)
        window?.rootViewController = ProbeVC()
        window?.makeKeyAndVisible()
        print("PROBE launched sup=\(env("PROBE_SUPPORTED") ?? "all") freeze=\(env("PROBE_FREEZE") ?? "0") delay=\(env("PROBE_FREEZE_DELAY") ?? "-")")
        return true
    }
}

_ = UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(AppDelegate.self))
