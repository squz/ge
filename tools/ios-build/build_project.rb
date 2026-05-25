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
#       'shaders/bgfx/metal',
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

module GE
  module IOS
    # Hard-coded engine source list, mirroring ge/Module.mk's ge/SRC_DIRECT.
    # Vendor sources (bgfx, bx, bimg, box2d, lunasvg, plutovg, sqlite3,
    # lz4, liteparser) are NOT compiled inline — they're prebuilt into
    # static libs under ge/prebuilt/ios-arm64/ via
    # `make ge/prebuild-vendor-ios-arm64` and linked from there. See
    # docs/vendor-prebuilds.md for the refresh workflow (T71).
    #
    # KEEP IN SYNC with Module.mk's ge/SRC_DIRECT — drift will cause
    # undefined-symbol errors at link time.
    GE_DIRECT_SOURCES = %w[
      src/Context.cpp
      src/Resource.cpp
      src/FileIO.cpp
      src/FontLoader_apple.mm
      src/BgfxContext.mm
      src/Signal.cpp
      src/SessionHost.mm
      src/sprite.cpp
      src/svg.cpp
      src/png.cpp
      src/text.cpp
      src/button.cpp
      src/sdl_input.cpp
      src/iap.cpp
      src/iap_apple.mm
      src/iap_apple.swift
      src/audio.cpp
      src/audio_apple.mm
      src/log.cpp
      src/log_apple.mm
      src/Immersive_stub.cpp
      src/CutoutInsets_stub.cpp
      src/Attitude_apple.mm
      src/render/DirectRenderHost.mm
      src/render/RefreshRateBoost_apple.mm
      tools/player_orientation_ios.mm
      vendor/src/sqlpipe.cpp
    ].freeze

    # iOS frameworks ge needs: SDL3 + bgfx/Metal + audio + StoreKit etc.
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
    # - bgfx/bx/bimg/box2d/{lunasvg,plutovg,sqlite3,lz4}_ge/liteparser:
    #   prebuilt for iOS arm64 under ge/prebuilt/ios-arm64/ (T71). Built
    #   via `make ge/prebuild-vendor-ios-arm64`; refreshed when bumping
    #   vendor submodules. The _ge suffix on lunasvg/plutovg/sqlite3/lz4
    #   avoids name collision with SDL3's own bundled plutosvg/plutovg
    #   and any system-installed sqlite3.
    LINKER_LIBS = %w[
      SDL3 SDL3_image SDL3_ttf
      freetype harfbuzz plutosvg plutovg
      bgfx bx bimg box2d
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
        deployment_target: '16.3',
        device_family: '1,2', # 1=iPhone, 2=iPad
        extra_defines: []
      )
        @app_name = app_name
        @bundle_id = bundle_id
        @marketing_version = marketing_version
        @build_number = build_number
        @display_name = display_name || app_name
        @team_id = team_id
        @signing_profile = signing_profile || "match AppStore #{bundle_id}"
        @deployment_target = deployment_target
        @device_family = device_family
        @extra_defines = extra_defines

        # Path defaults — assume the consumer's invocation is from the
        # repo root (e.g. `bundle exec ruby ios/project.rb` from the
        # multimaze2 root).
        @project_root = File.expand_path(project_root || Dir.pwd)
        @ios_dir      = File.expand_path(ios_dir      || File.join(@project_root, 'ios'))
        @ge_root      = File.expand_path(ge_root      || File.join(@project_root, 'ge'))
        @info_plist   = info_plist      || File.join(@ios_dir, 'Info.plist')
        @assets_xcassets = assets_xcassets || File.join(@ios_dir, 'Assets.xcassets')

        @game_sources_abs = game_sources.map { |p| File.expand_path(p, @project_root) }
        @resources = resources # array of relative-to-project_root paths (files or dirs)

        @xcodeproj_path = File.join(@ios_dir, "#{@app_name}.xcodeproj")
      end

      def generate
        FileUtils.mkdir_p(@ios_dir)
        @project = Xcodeproj::Project.new(@xcodeproj_path)

        add_app_target!
        add_build_configurations!
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
          # Device-only. The simulator path requires a matching simulator
          # runtime which isn't always installed on CI macOS runners, and
          # App Store IPAs don't include simulator slices anyway. Local
          # dev that needs the simulator can override via Xcode UI or
          # consumer-side `extra_settings` once that surface lands.
          'SUPPORTED_PLATFORMS' => 'iphoneos',

          # Squz signing — Manual + match-installed profile + Apple
          # Distribution: Squz Pty Ltd. See [[squz-cert-policy]].
          'CODE_SIGN_STYLE' => 'Manual',
          'DEVELOPMENT_TEAM' => @team_id,
          'CODE_SIGN_IDENTITY' => 'Apple Distribution',
          'PROVISIONING_PROFILE_SPECIFIER' => @signing_profile,

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

          # Library search paths:
          # - SDL3 prebuilt xcframeworks (device vs simulator slices).
          # - Vendor prebuilt static libs from T71's prebuild step
          #   (`make ge/prebuild-vendor-ios-arm64`), iphoneos only.
          'LIBRARY_SEARCH_PATHS[sdk=iphoneos*]' => [
            File.join(relative_to_ios(@ge_root), 'vendor/sdl3/lib/ios-arm64'),
            File.join(relative_to_ios(@ge_root), 'prebuilt/ios-arm64'),
          ].map { |p| "\"$(SRCROOT)/#{p}\"" }.join(' '),
          'LIBRARY_SEARCH_PATHS[sdk=iphonesimulator*]' => File.join(relative_to_ios(@ge_root), 'vendor/sdl3/lib/ios-arm64-simulator'),

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
          File.join(ge_rel, 'include'),
          File.join(ge_rel, 'vendor/include'),
          # Non-prebuilt, non-submodule deps (header-only or always
          # consumed inline by engine sources).
          File.join(ge_rel, 'vendor/github.com/gabime/spdlog/include'),
          File.join(ge_rel, 'vendor/github.com/chriskohlhoff/asio/include'),
          File.join(ge_rel, 'vendor/github.com/libsdl-org/SDL/include'),
          File.join(ge_rel, 'vendor/github.com/libsdl-org/SDL_ttf/external/freetype/include'),
          File.join(ge_rel, 'vendor/sdl3/include'),
          # Vendor prebuilts (T71): public headers lifted from each
          # vendor submodule into ge/headers/<vendor>/include/. Same
          # consumer #include paths as before — consumers see
          # `<bgfx/bgfx.h>`, `<bx/bx.h>`, etc. unchanged. The lifted
          # tree means consumer CI doesn't need to recurse-init ge's
          # submodules to compile.
          File.join(ge_rel, 'headers/liteparser/include'),
          File.join(ge_rel, 'headers/lunasvg/include'),
          File.join(ge_rel, 'headers/plutovg/include'),
          File.join(ge_rel, 'headers/box2d/include'),
          File.join(ge_rel, 'headers/bx/include'),
          File.join(ge_rel, 'headers/bx/include/compat/ios'),
          File.join(ge_rel, 'headers/bimg/include'),
          File.join(ge_rel, 'headers/bgfx/include'),
        ].map { |p| "\"$(SRCROOT)/#{p}\"" }
      end

      # ── sources ─────────────────────────────────────────────────────────

      def add_sources!
        # Game sources — consumer's own .cpp / .mm files.
        @game_sources_abs.each do |path|
          add_source_file(path, vendor: false)
        end

        # ge engine sources. (Vendor libs — bgfx, bx, bimg, box2d,
        # lunasvg, plutovg, sqlite3, lz4, liteparser — are NOT compiled
        # inline; they're prebuilt static libs under
        # ge/prebuilt/ios-arm64/ and linked via LINKER_LIBS. See T71.)
        GE_DIRECT_SOURCES.each do |rel|
          add_source_file(File.join(@ge_root, rel), vendor: false)
        end
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

      def add_resources!
        # Info.plist is referenced via INFOPLIST_FILE setting, NOT as a
        # resource. Skip it here.

        # Assets.xcassets (app-icon catalog from `make ge/app-icons`).
        if File.exist?(@assets_xcassets)
          ref = file_ref_for(@assets_xcassets)
          @app_target.add_resources([ref])
        end

        # Game resources — each entry is a path relative to project root.
        # Files become single PBXFileReferences; directories are recursed
        # and added as folder references (preserves bundle subdir layout).
        @resources.each do |rel|
          abs = File.expand_path(rel, @project_root)
          if File.directory?(abs)
            ref = folder_ref_for(abs)
            @app_target.add_resources([ref])
          elsif File.file?(abs)
            ref = file_ref_for(abs)
            @app_target.add_resources([ref])
          else
            warn "resource not found: #{abs}"
          end
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
        flags = LINKER_LIBS.map { |l| "-l#{l}" } + ['-lobjc']
        @app_target.build_configurations.each do |config|
          existing = config.build_settings['OTHER_LDFLAGS'] || ''
          config.build_settings['OTHER_LDFLAGS'] = (existing.split + flags).join(' ')
        end
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

      def folder_ref_for(abs_path)
        rel = relative_to_ios(abs_path)
        ref = @project.main_group.new_file(rel)
        ref.source_tree = 'SOURCE_ROOT'
        ref.path = rel
        ref.last_known_file_type = 'folder'
        ref
      end
    end
  end
end
