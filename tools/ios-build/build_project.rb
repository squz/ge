# ge iOS app-project generator — replaces ge/tools/ios-template/CMakeLists.txt.in.
#
# Consumer pattern (e.g. multimaze2/ios/project.rb):
#
#   require_relative '../ge/tools/ios-build/build_project'
#
#   GE::IOS::ProjectBuilder.new(
#     app_name: 'MultiMaze',
#     bundle_id: 'com.squz.multimaze',
#     marketing_version: '3.0.0',
#     build_number: '1',
#     game_sources: Dir.glob('src/*.cpp') + Dir.glob('src/*.mm'),
#     resources: [
#       'data/atlas.png',
#       'data/quadrants.png',
#       'data/menu',
#       'data/fonts',
#       'data/audio',
#       'shaders/metal',
#     ],
#   ).generate
#
# Consumer's `make ios` becomes `bundle exec ruby ios/project.rb`.
#
# Output: ios/<AppName>.xcodeproj/ (gitignored).
#
# Why Ruby + xcodeproj (not CMake's Xcode generator, not XcodeGen YAML):
# - CMake's Xcode generator hardcodes CODE_SIGN_STYLE = Automatic, which
#   under CLI archive picks the personal dev cert instead of the squz
#   distribution cert. Working around with fastlane xcargs broke the
#   export step with "Unknown Distribution Error".
# - XcodeGen YAML's main advantage (human-authorability) doesn't apply
#   when we're generating programmatically anyway. xcodeproj is a Ruby
#   library — same language as fastlane, full programmatic control over
#   the .pbxproj data model, no DSL ceiling.
# See [[squz-cert-policy]] for signing requirements; T35 for the pivot.

require 'xcodeproj'
require 'pathname'
require 'fileutils'
require 'set'
require 'shellwords'

module GE
  module IOS
    # Sources from ge that the consumer still compiles inline.
    #
    # Almost all engine sources moved into the prebuilt libge.a (T71
    # Phase 2) — see tools/prebuild-vendor-ios-arm64.sh for the source
    # list cooked into the .a. The Swift file stays here because Swift
    # integration with a static lib still needs the consumer's
    # bridging-header config, and the cost (~1 Swift file) is trivial.
    GE_DIRECT_SOURCES = %w[
      src/iap_apple.swift
    ].freeze
    GE_ENGINE_MODES = %i[prebuilt source].freeze

    # iOS frameworks ge needs: SDL3 + sokol/Metal + audio + StoreKit etc.
    FRAMEWORKS = %w[
      Foundation UIKit
      Metal MetalKit
      QuartzCore IOKit IOSurface
      CoreGraphics CoreServices CoreFoundation CoreBluetooth
      CoreMedia CoreVideo
      AudioToolbox AVFoundation
      GameController CoreHaptics CoreMotion
      VideoToolbox Security OpenGLES ImageIO CoreText StoreKit
    ].freeze

    # Linker libs:
    # - SDL3 et al.: prebuilt xcframeworks under ge/vendor/sdl3/ (legacy).
    # - ge: engine itself, prebuilt under ge/prebuilt/ios-arm64/libge.a
    #   (T71 Phase 2). Listed first so ge symbols are resolved before
    #   vendor libs (e.g. ge::log uses spdlog template instantiations).
    # - box2d/{lunasvg,plutovg,sqlite3,lz4}_ge/liteparser:
    #   prebuilt under ge/prebuilt/ios-arm64/ (T71). Built via
    #   `make prebuild`; refreshed when bumping vendor submodules. The _ge
    #   suffix on lunasvg/plutovg/sqlite3/lz4 avoids name collision with
    #   SDL3's own bundled plutosvg/plutovg and any system-installed sqlite3.
    # - T38: bgfx/bx/bimg removed (engine migrated to sokol_gfx; the
    #   single-header sokol_gfx.h is included at SOKOL_IMPL time inside
    #   libge.a so no separate prebuilt static lib).
    LINKER_LIBS = %w[
      ge
      SDL3 SDL3_image SDL3_ttf
      freetype harfbuzz plutosvg plutovg
      box2d
      lunasvg_ge plutovg_ge sqlite3_ge lz4_ge liteparser
    ].freeze

    class ProjectBuilder
      attr_reader :project, :app_target

      def initialize(
        app_name:,
        bundle_id:,
        marketing_version:,
        build_number:,
        game_sources:,
        ios_dir: nil,
        project_root: nil,
        ge_root: nil,
        info_plist: nil,
        assets_xcassets: nil,
        resources: [],
        display_name: nil,
        team_id: ENV.fetch('APPLE_TEAM_ID'),
        signing_profile: nil,
        # Device signing mode:
        #   :appstore (default) — Manual + match AppStore profile + Apple
        #     Distribution (Squz). For release/TestFlight via gym.
        #   :development — Automatic + Apple Development. For installing on a
        #     developer's own registered test devices; pair the build with
        #     `xcodebuild -allowProvisioningUpdates`. Per-developer cert, no
        #     match. Resolved from GE_IOS_SIGNING when not passed explicitly,
        #     so the dev make-lane just sets the env (no consumer edit).
        signing: nil,
        deployment_target: '16.3',
        device_family: '1,2', # 1=iPhone, 2=iPad
        extra_defines: [],
        # :prebuilt links ge/prebuilt/*/libge.a (release/CI default).
        # :source compiles ge C/C++/ObjC++ sources into the app target while
        # still linking vendor prebuilts. Use for local ge iteration so source
        # edits don't require re-cooking libge.a first.
        engine_mode: :prebuilt,
        # When true, the generated xcodeproj supports both iphoneos and
        # iphonesimulator destinations — LIBRARY_SEARCH_PATHS picks the
        # right prebuilt subdir per SDK, and code-sign settings relax
        # for sim builds (no provisioning profile needed). Default false
        # for backwards compatibility with existing distribution
        # consumers (e.g. multimaze2's release pipeline) (🎯T73.2).
        simulator: false
      )
        @app_name = app_name
        @bundle_id = bundle_id
        @marketing_version = marketing_version
        @build_number = build_number
        @display_name = display_name || app_name
        @team_id = team_id
        @signing_profile = signing_profile || "match AppStore #{bundle_id}"
        @signing = (signing || ENV.fetch('GE_IOS_SIGNING', 'appstore')).to_sym
        unless %i[appstore development].include?(@signing)
          raise ArgumentError, "signing must be :appstore or :development (got #{@signing})"
        end
        @deployment_target = deployment_target
        @device_family = device_family
        @extra_defines = extra_defines
        @engine_mode = engine_mode.to_sym
        unless GE_ENGINE_MODES.include?(@engine_mode)
          raise ArgumentError, "engine_mode must be one of #{GE_ENGINE_MODES.join(', ')}"
        end
        @simulator = simulator

        # Path defaults — assume the consumer's invocation is from the
        # repo root (e.g. `bundle exec ruby ios/project.rb` from the
        # multimaze2 root).
        @project_root = File.expand_path(project_root || Dir.pwd)
        @ios_dir      = File.expand_path(ios_dir      || File.join(@project_root, 'ios'))
        @ge_root      = File.expand_path(ge_root      || File.join(@project_root, 'ge'))
        @info_plist   = info_plist      || File.join(@ios_dir, 'Info.plist')
        @assets_xcassets = assets_xcassets || File.join(@ios_dir, 'Assets.xcassets')
        @storekit_config = File.join(@ios_dir, 'StoreKit.storekit')

        @game_sources_abs = game_sources.map { |p| File.expand_path(p, @project_root) }
        @resources = resources # array of relative-to-project_root paths (files or dirs)

        @xcodeproj_path = File.join(@ios_dir, "#{@app_name}.xcodeproj")
      end

      def generate
        FileUtils.mkdir_p(@ios_dir)
        @project = Xcodeproj::Project.new(@xcodeproj_path)

        add_app_target!
        add_build_configurations!
        add_shader_codegen_phase!
        add_prebuilt_cook_verify_phase!
        add_sources!
        add_resources!
        add_frameworks!
        add_linker_settings!

        @project.save
        puts "Generated #{@xcodeproj_path}"
      end

      private

      # ── target ──────────────────────────────────────────────────────────

      def add_app_target!
        @app_target = @project.new_target(
          :application,
          @app_name,
          :ios,
          @deployment_target
        )
        # Force the app target's product type bundle ID so Info.plist's
        # $(PRODUCT_BUNDLE_IDENTIFIER) substitution resolves to our value.
        @app_target.build_configurations.each do |config|
          config.build_settings['PRODUCT_BUNDLE_IDENTIFIER'] = @bundle_id
        end
      end

      # ── build settings ──────────────────────────────────────────────────

      def add_build_configurations!
        common_settings = {
          'PRODUCT_NAME' => @app_name,
          'INFOPLIST_FILE' => relative_to_ios(@info_plist),
          'MARKETING_VERSION' => @marketing_version,
          'CURRENT_PROJECT_VERSION' => @build_number,
          'IPHONEOS_DEPLOYMENT_TARGET' => @deployment_target,
          'TARGETED_DEVICE_FAMILY' => @device_family,
          # SUPPORTED_PLATFORMS: distribution defaults to iphoneos only
          # (App Store IPAs don't include simulator slices); dev mode
          # (simulator: true) supports both so the same xcodeproj drives
          # device and sim builds without regenerating.
          'SUPPORTED_PLATFORMS' => @simulator ? 'iphoneos iphonesimulator' : 'iphoneos',

          # Device signing (iphoneos). @signing picks the mode:
          #   :appstore    — Manual + match AppStore profile + Apple Distribution
          #                  (Squz). Release/TestFlight via gym. [[squz-cert-policy]]
          #   :development — Automatic + Apple Development. Installs on a dev's own
          #                  registered devices; build with
          #                  `xcodebuild -allowProvisioningUpdates` so Xcode mints
          #                  the dev cert + Team Provisioning Profile and registers
          #                  connected devices. Per-developer cert, no match.
          **(@signing == :development ?
            {
              'CODE_SIGN_STYLE'                => 'Automatic',
              'DEVELOPMENT_TEAM'               => @team_id,
              'CODE_SIGN_IDENTITY'             => 'Apple Development',
              'PROVISIONING_PROFILE_SPECIFIER' => '',
            } : {
              'CODE_SIGN_STYLE'                => 'Manual',
              'DEVELOPMENT_TEAM'               => @team_id,
              'CODE_SIGN_IDENTITY'             => 'Apple Distribution',
              'PROVISIONING_PROFILE_SPECIFIER' => @signing_profile,
            }),
          # Simulator code-sign settings (both modes): no profile required.
          # Xcode accepts `--` (any identity) for sim and skips entitlements.
          'CODE_SIGN_IDENTITY[sdk=iphonesimulator*]' => '-',
          'CODE_SIGN_STYLE[sdk=iphonesimulator*]' => 'Automatic',
          'PROVISIONING_PROFILE_SPECIFIER[sdk=iphonesimulator*]' => '',

          # Languages
          'CLANG_CXX_LANGUAGE_STANDARD' => 'c++20',
          'CLANG_CXX_LIBRARY' => 'libc++',
          'GCC_C_LANGUAGE_STANDARD' => 'c11',
          'SWIFT_VERSION' => '5.0',
          'SWIFT_OPTIMIZATION_LEVEL' => '-Onone',

          # Swift ↔ ObjC bridge for ge::iap (StoreKit 2 backend lives in
          # iap_apple.swift; iap_apple.mm calls it via the bridging header).
          'SWIFT_OBJC_BRIDGING_HEADER' => File.join(relative_to_ios(@ge_root), 'src/iap_apple_bridge.h'),
          'CLANG_ENABLE_MODULES' => 'YES',

          # Headers
          'HEADER_SEARCH_PATHS' => header_search_paths.join(' '),
          'USER_HEADER_SEARCH_PATHS' => '',

          # Preprocessor — single-binary distribution build (GE_DIRECT_ONLY
          # excludes wire/networking; GE_IOS is the platform marker).
          'GCC_PREPROCESSOR_DEFINITIONS' => [
            'ASIO_STANDALONE',
            'GE_IOS',
            'GE_DIRECT_ONLY',
            'SQLITE_ENABLE_SESSION',
            'SQLITE_ENABLE_PREUPDATE_HOOK',
            'SQLITE_ENABLE_DESERIALIZE',
            'BX_CONFIG_DEBUG=0',
            'BIMG_CONFIG_DECODE_ENABLE=0',
            'LUNASVG_BUILD_STATIC',
            'PLUTOVG_BUILD_STATIC',
            *@extra_defines,
          ].join(' '),

          # Library search paths — SDL3 prebuilt + ge/vendor prebuilt
          # static libs per SDK slice. Both SDK variants pick from the
          # parallel directory tree produced by `make prebuild`
          # (🎯T73.1 / 🎯T73.2).
          'LIBRARY_SEARCH_PATHS[sdk=iphoneos*]' => [
            File.join(relative_to_ios(@ge_root), 'vendor/sdl3/lib/ios-arm64'),
            File.join(relative_to_ios(@ge_root), 'prebuilt/ios-arm64'),
          ].map { |p| "\"$(SRCROOT)/#{p}\"" }.join(' '),
          'LIBRARY_SEARCH_PATHS[sdk=iphonesimulator*]' => [
            File.join(relative_to_ios(@ge_root), 'vendor/sdl3/lib/ios-arm64-simulator'),
            File.join(relative_to_ios(@ge_root), 'prebuilt/ios-arm64-simulator'),
          ].map { |p| "\"$(SRCROOT)/#{p}\"" }.join(' '),

          # iOS Simulator on Apple Silicon defaults to dual-arch
          # (arm64 + x86_64); ge's sim prebuilts ship arm64 only, so
          # exclude x86_64 here. macOS dev box → arm64 sim arch matches
          # everywhere. Re-enable x86_64 only if ge ever ships a fat sim
          # prebuilt for Intel macOS hosts.
          'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'x86_64',

          # Misc Xcode hygiene
          'ASSETCATALOG_COMPILER_APPICON_NAME' => 'AppIcon',
          'ENABLE_GPU_API_VALIDATION' => 'NO',
          'ENABLE_BITCODE' => 'NO',
        }

        @app_target.build_configurations.each do |config|
          config.build_settings.merge!(common_settings)
          config.build_settings['ONLY_ACTIVE_ARCH'] = (config.name == 'Debug') ? 'YES' : 'NO'
        end
      end

      def header_search_paths
        ge_rel = relative_to_ios(@ge_root)
        proj_rel = relative_to_ios(@project_root)
        [
          File.join(proj_rel, 'src'),
          File.join(proj_rel, 'include'),
          File.join(proj_rel, 'vendor/include'),   # consumer's vendored headers (harmless if absent)
          File.join(ge_rel, 'include'),
          File.join(ge_rel, 'vendor/include'),
          # Header-only deps + SDL3 stack — lifted from their submodules
          # into ge/headers/ so consumer CI doesn't need recursive
          # submodule init (T71 follow-up; original T71 missed these).
          # `vendor/sdl3/include` still on the path because it holds the
          # checked-in SDL3_image / SDL3_ttf header subdirs (siblings to
          # the prebuilt static libs under vendor/sdl3/lib/).
          File.join(ge_rel, 'headers/spdlog/include'),
          File.join(ge_rel, 'headers/asio/include'),
          File.join(ge_rel, 'headers/sdl3/include'),
          File.join(ge_rel, 'vendor/sdl3/include'),
          # Vendor prebuilts (T71): public headers lifted from each
          # vendor submodule into ge/headers/<vendor>/include/. The
          # lifted tree means consumer CI doesn't need to recurse-init
          # ge's submodules to compile.
          # T38: bx/bimg/bgfx header paths dropped (sokol_gfx is the
          # rendering backend now; vendor/github.com/floooh/sokol is on
          # the path below as a single-header drop-in).
          File.join(ge_rel, 'headers/liteparser/include'),
          File.join(ge_rel, 'headers/lunasvg/include'),
          File.join(ge_rel, 'headers/plutovg/include'),
          File.join(ge_rel, 'headers/box2d/include'),
          # T38: sokol_gfx.h + sokol-shdc-generated ge_sprite.h + the
          # consumer's own sokol-shdc-generated app shader headers.
          # Both shader output dirs are populated by the Compile Sokol
          # Shaders build phase (see add_shader_codegen_phase!).
          File.join(ge_rel, 'vendor/github.com/floooh/sokol'),
          File.join(ge_rel, 'build/ge/shaders'),
          File.join(proj_rel, 'build/shaders'),
        ].map { |p| "\"$(SRCROOT)/#{p}\"" }
      end

      # ── sources ─────────────────────────────────────────────────────────

      def add_sources!
        # Game sources — consumer's own .cpp / .mm files.
        @game_sources_abs.each do |path|
          add_source_file(path, vendor: false)
        end

        # ge engine sources. In release/CI mode this is just the Swift bridge;
        # libge.a supplies the C++ engine. In source mode, compile the same
        # direct iOS source list tools/prebuild.sh uses, but keep vendor libs
        # prebuilt.
        ge_source_rels.each do |rel|
          add_source_file(File.join(@ge_root, rel), vendor: false)
        end
      end

      def ge_source_rels
        return GE_DIRECT_SOURCES if @engine_mode == :prebuilt

        manifest = File.join(@ge_root, 'tools/ge-sources.mk')
        raise "ge source manifest missing: #{manifest}" unless File.exist?(manifest)

        out = `make -s -f #{Shellwords.escape(manifest)} print-direct-ios`
        raise "failed to read ge direct iOS sources from #{manifest}" unless $?.success?

        GE_DIRECT_SOURCES + out.lines.map(&:strip).reject(&:empty?)
      end

      # xcodeproj's `add_file_references(refs, compiler_flags)` takes a
      # string for `compiler_flags` (NOT a hash). Passing a hash silently
      # stores it as the value of COMPILER_FLAGS — Xcode then chokes
      # opening the project with "-[__NSDictionaryM length]:
      # unrecognized selector". This wrapper centralises the
      # path → file_ref → build-file linkage and applies COMPILER_FLAGS
      # correctly by mutating the build_file's `settings` hash directly.
      def add_source_file(abs_path, vendor:)
        file_ref = file_ref_for(abs_path)
        build_file = @app_target.source_build_phase.add_file_reference(file_ref, true)
        flags = compiler_flags_for(abs_path, vendor: vendor)
        build_file.settings = { 'COMPILER_FLAGS' => flags } unless flags.empty?
      end

      def compiler_flags_for(_path, vendor:)
        # Silence vendor-code warnings — they're not ours to fix. Today
        # only sqlpipe.cpp (vendor/src/sqlpipe.cpp) passes vendor: true;
        # other vendor C/C++ libs are linked from ge/prebuilt/ios-arm64/.
        return '-Wno-everything' if vendor
        ''
      end

      # ── resources ───────────────────────────────────────────────────────

      # Bundles consumer-declared resources into the app, preserving each
      # resource's project-relative path inside the .app/ bundle. A consumer
      # declaration of "data/menu" lands at ".app/data/menu/...", matching
      # how ge::resource("data/menu/x.png") resolves at runtime.
      #
      # Mechanics: Xcode's standard "Copy Bundle Resources" phase copies
      # each file keyed by basename — so a single phase can't preserve
      # subpaths. Instead, this method walks each resource directory and
      # creates one PBXCopyFilesBuildPhase per unique relative parent
      # directory, with dst_subfolder_spec = 7 (Resources, which on iOS
      # resolves to the .app bundle root) and dst_path = <rel parent>.
      # Top-level files (those whose parent is the project root) keep
      # going through the regular Resources phase.
      #
      # This mirrors what CMake's MACOSX_PACKAGE_LOCATION property did in
      # the pre-T35 build, so consumers migrating from CMake see identical
      # bundle layout from identical declarations. T72 retired this
      # contract after the basename-flattening regression broke
      # multimaze2's TestFlight 3.0.0 ship.
      def add_resources!
        # Info.plist is referenced via INFOPLIST_FILE setting, NOT as a
        # resource. Skip it here.

        # Assets.xcassets (app-icon catalog from `make ge/app-icons`).
        # Stays in the regular Resources phase — Xcode treats .xcassets
        # specially (asset-catalog compilation), and the bundle output
        # path is determined by the catalog itself.
        if File.exist?(@assets_xcassets)
          ref = file_ref_for(@assets_xcassets)
          @app_target.add_resources([ref])
        end

        # Game resources — each entry is a path relative to project root.
        @resources.each do |rel|
          abs = File.expand_path(rel, @project_root)
          if File.directory?(abs)
            add_resource_directory!(abs)
          elsif File.file?(abs)
            add_resource_file!(abs)
          else
            warn "resource not found: #{abs}"
          end
        end

        # 🎯T86: on simulator (dev) builds, bundle StoreKit.storekit if the
        # consumer has one. Its mere presence in the bundle makes ge::iap's
        # auto-mode resolve to "stub" (T74) — in-memory entitlements, zero
        # StoreKit calls, so no App Store login dialogue during sim
        # iteration. Distribution/device builds (@simulator false) don't
        # bundle it and stay on the real platform StoreKit backend.
        #
        # NB: this only triggers stub, which does not read the file's
        # contents. Full LocalStore (SKTestSession against the .storekit's
        # fake products) is deferred — see 🎯T85: StoreKitTest transitively
        # requires XCTest, which can't load in a plain (non-XCTest) app
        # process, so SKTestSession isn't viable here yet.
        if @simulator && File.exist?(@storekit_config)
          @app_target.add_resources([file_ref_for(@storekit_config)])
        end
      end

      # Walks abs_dir recursively and places each file at its
      # project-relative path in the bundle.
      def add_resource_directory!(abs_dir)
        Pathname.new(abs_dir).find do |path|
          next if path.directory?
          add_resource_file!(path.to_s)
        end
      end

      # Adds a single file at its project-relative path. Files at the
      # project root go through the standard Resources phase; files
      # nested under one or more directories go through a per-directory
      # Copy Files phase with dst_path matching the relative parent.
      def add_resource_file!(abs_path)
        rel = Pathname.new(File.expand_path(abs_path))
          .relative_path_from(Pathname.new(@project_root))
          .to_s
        rel_parent = File.dirname(rel)
        file_ref = file_ref_for(abs_path)
        if rel_parent == '.' || rel_parent.empty?
          @app_target.add_resources([file_ref])
        else
          phase = copy_files_phase_for(rel_parent)
          phase.add_file_reference(file_ref, true)
        end
      end

      # Lazily creates (and memoises) one Copy Files build phase per
      # unique destination subpath. dst_subfolder_spec '7' = Resources;
      # on iOS that resolves to the .app bundle root, so dst_path is
      # interpreted relative to .app/.
      def copy_files_phase_for(dst_path)
        @copy_files_phases ||= {}
        @copy_files_phases[dst_path] ||= begin
          phase = @app_target.new_copy_files_build_phase("Copy Resources (#{dst_path})")
          phase.dst_path = dst_path
          phase.dst_subfolder_spec = '7'
          phase
        end
      end

      # ── frameworks + linker ─────────────────────────────────────────────

      def add_frameworks!
        # Xcodeproj's new_target(:application, ..., :ios, ...) already
        # adds Foundation.framework into the iOS group + frameworks build
        # phase. Compute the already-linked set and skip duplicates.
        already_linked = @app_target.frameworks_build_phase.file_display_names.to_set
        FRAMEWORKS.each do |name|
          file_name = "#{name}.framework"
          next if already_linked.include?(file_name)
          ref = @project.frameworks_group.new_reference("System/Library/Frameworks/#{file_name}")
          ref.source_tree = 'SDKROOT'
          @app_target.frameworks_build_phase.add_file_reference(ref)
        end
      end

      def add_linker_settings!
        # Linker flags for the prebuilt SDL3 + ancillary libs that don't
        # compile from source.
        libs = @engine_mode == :source ? LINKER_LIBS.reject { |l| l == 'ge' } : LINKER_LIBS
        flags = libs.map { |l| "-l#{l}" } + ['-lobjc']
        @app_target.build_configurations.each do |config|
          existing = config.build_settings['OTHER_LDFLAGS'] || ''
          config.build_settings['OTHER_LDFLAGS'] = (existing.split + flags).join(' ')
        end
      end

      # ── shader codegen (T38) ───────────────────────────────────────────
      #
      # sokol-shdc invocation: takes annotated GLSL (.glsl with @vs/@fs/
      # @program blocks) and emits one C header per program with embedded
      # MSL+GLSL bytecode for every backend listed in `-l`. The header
      # gets #included by ge::sprite / consumer renderers via the
      # build/{ge,}/shaders include paths added in header_search_paths.
      #
      # We add this as a script build phase that runs BEFORE Compile
      # Sources so the headers exist when src/sprite.cpp + consumer's
      # Renderer.cpp compile.
      #
      # ge's own internal shaders (only ge_sprite.h post-T38) get codegen'd
      # into build/ge/shaders/. The consumer's shaders (e.g. tiltbuggy's
      # simple.glsl) get codegen'd into build/shaders/.
      def add_shader_codegen_phase!
        ge_rel = relative_to_ios(@ge_root)
        proj_rel = relative_to_ios(@project_root)
        shdc_path = File.join(ge_rel, 'vendor/github.com/floooh/sokol-tools-bin/bin/osx_arm64/sokol-shdc')
        ge_shader_src_dir = File.join(ge_rel, 'src/render/shaders')
        ge_shader_out_dir = File.join(ge_rel, 'build/ge/shaders')
        app_shader_src_dir = File.join(proj_rel, 'shaders')        # consumer's shaders/ dir
        app_shader_out_dir = File.join(proj_rel, 'build/shaders')  # matches Module.mk + header_search_paths

        script = <<~SH
          set -euo pipefail
          SHDC="${SRCROOT}/#{shdc_path}"
          if [ ! -x "$SHDC" ]; then
            echo "error: sokol-shdc not found or not executable at $SHDC" >&2
            echo "  run: git submodule update --init vendor/github.com/floooh/sokol-tools-bin" >&2
            exit 1
          fi
          # metal_sim (🎯T84): the iOS Simulator reports SG_BACKEND_METAL_SIMULATOR;
          # without a metal_sim slot the generated shader_desc returns NULL there
          # and sg_make_shader aborts. Keep in sync with Module.mk's
          # ge/SOKOL_SHDC_LANGS and tools/prebuild.sh.
          LANGS="metal_macos:metal_ios:metal_sim:glsl300es"

          # ge engine shaders → build/ge/shaders/
          GE_SRC="${SRCROOT}/#{ge_shader_src_dir}"
          GE_OUT="${SRCROOT}/#{ge_shader_out_dir}"
          mkdir -p "$GE_OUT"
          for glsl in "$GE_SRC"/*.glsl; do
            [ -e "$glsl" ] || continue
            name="$(basename "$glsl" .glsl)"
            out="$GE_OUT/$name.h"
            if [ ! -e "$out" ] || [ "$glsl" -nt "$out" ]; then
              echo "sokol-shdc: $name (ge)"
              "$SHDC" -i "$glsl" -o "$out" -l "$LANGS" -f sokol
            fi
          done

          # Consumer app shaders → build/shaders/
          APP_SRC="${SRCROOT}/#{app_shader_src_dir}"
          APP_OUT="${SRCROOT}/#{app_shader_out_dir}"
          if [ -d "$APP_SRC" ]; then
            mkdir -p "$APP_OUT"
            for glsl in "$APP_SRC"/*.glsl; do
              [ -e "$glsl" ] || continue
              name="$(basename "$glsl" .glsl)"
              out="$APP_OUT/$name.h"
              if [ ! -e "$out" ] || [ "$glsl" -nt "$out" ]; then
                echo "sokol-shdc: $name (app)"
                "$SHDC" -i "$glsl" -o "$out" -l "$LANGS" -f sokol
              fi
            done
          fi
        SH

        phase = @app_target.new_shell_script_build_phase('Compile Sokol Shaders')
        phase.shell_script = script
        phase.shell_path = '/bin/bash'
        # Move it to be the first phase so generated headers exist before
        # Compile Sources runs. (Verify-cook phase unshifts after this and
        # ends up first — cook before shaders before compile.)
        @app_target.build_phases.delete(phase)
        @app_target.build_phases.unshift(phase)
      end

      # Hard co-cook gate for iOS (parity with cmake/android-arm64.cmake).
      # Every prebuilt .a must match prebuilt/<platform>/cook.json from a
      # single full tools/prebuild.sh cook. Runs every Xcode build, even if
      # the user opened the .xcodeproj without going through make.
      def add_prebuilt_cook_verify_phase!
        ge_rel = relative_to_ios(@ge_root)
        script = <<~SH
          set -euo pipefail
          GE_ROOT="${SRCROOT}/#{ge_rel}"
          VERIFY="${GE_ROOT}/tools/verify-cook.py"
          if [ ! -f "$VERIFY" ]; then
            echo "error: missing $VERIFY" >&2
            exit 1
          fi
          # PLATFORM_NAME is iphoneos | iphonesimulator (set by Xcode).
          case "${PLATFORM_NAME:-}" in
            iphonesimulator) KEY=ios-arm64-simulator ;;
            iphoneos|*)      KEY=ios-arm64 ;;
          esac
          PREBUILT="${GE_ROOT}/prebuilt/${KEY}"
          echo "ge: verifying prebuilt cook for ${KEY}…"
          python3 "$VERIFY" "$PREBUILT"
        SH

        phase = @app_target.new_shell_script_build_phase('Verify prebuilt cook')
        phase.shell_script = script
        phase.shell_path = '/bin/bash'
        # Always run (no dependency analysis) — a hand-copied .a must fail.
        phase.always_out_of_date = '1' if phase.respond_to?(:always_out_of_date=)
        @app_target.build_phases.delete(phase)
        @app_target.build_phases.unshift(phase)
      end

      # ── path helpers ────────────────────────────────────────────────────

      # Return path relative to the .xcodeproj's parent dir (ios/). Used
      # for the SRCROOT-relative paths in build settings and file refs.
      def relative_to_ios(abs_path)
        Pathname.new(File.expand_path(abs_path))
          .relative_path_from(Pathname.new(File.expand_path(@ios_dir)))
          .to_s
      end

      def file_ref_for(abs_path)
        rel = relative_to_ios(abs_path)
        ref = @project.main_group.new_reference(rel)
        ref.source_tree = 'SOURCE_ROOT'
        ref.path = rel
        ref
      end
    end
  end
end
