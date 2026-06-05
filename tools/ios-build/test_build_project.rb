#!/usr/bin/env ruby
# Regression test for ge/tools/ios-build/build_project.rb resource
# bundling (T72). Verifies that a declared resource directory `a/b/c`
# lands at `.app/a/b/c/` rather than `.app/c/` (the basename-flatten
# regression that broke multimaze2's TestFlight 3.0.0 ship).
#
# Run: `bundle exec ruby tools/ios-build/test_build_project.rb`

require 'minitest/autorun'
require 'tmpdir'
require 'fileutils'
require 'pathname'

require_relative 'build_project'

class BuildProjectTest < Minitest::Test
  def setup
    @tmp = Dir.mktmpdir('ge-build-project-test-')
    @project_root = @tmp
    @ios_dir = File.join(@tmp, 'ios')
    @ge_root = File.join(@tmp, 'ge')
    FileUtils.mkdir_p(@ios_dir)
    FileUtils.mkdir_p(@ge_root)

    # Bare source file so the builder has something to compile.
    @src = File.join(@tmp, 'src', 'main.cpp')
    FileUtils.mkdir_p(File.dirname(@src))
    File.write(@src, "// stub\n")

    # Stub Swift bridging header location referenced by build settings.
    FileUtils.mkdir_p(File.join(@ge_root, 'src'))
    File.write(File.join(@ge_root, 'src/iap_apple_bridge.h'), "// stub\n")

    # Stub Swift source referenced by GE_DIRECT_SOURCES.
    File.write(File.join(@ge_root, 'src/iap_apple.swift'), "// stub\n")

    FileUtils.mkdir_p(File.join(@ge_root, 'tools'))
    File.write(
      File.join(@ge_root, 'tools/ge-sources.mk'),
      "print-direct-ios:\n\t@printf '%s\\n' src/debug.cpp src/SessionHost.mm vendor/src/sqlpipe.cpp\n"
    )
    %w[src/debug.cpp src/SessionHost.mm vendor/src/sqlpipe.cpp].each do |rel|
      abs = File.join(@ge_root, rel)
      FileUtils.mkdir_p(File.dirname(abs))
      File.write(abs, "// stub\n")
    end

    ENV['APPLE_TEAM_ID'] ||= 'TESTTEAMID'
  end

  def teardown
    FileUtils.remove_entry(@tmp) if @tmp && File.directory?(@tmp)
  end

  def make_resource(rel)
    abs = File.join(@project_root, rel)
    FileUtils.mkdir_p(File.dirname(abs))
    File.write(abs, "stub\n")
    abs
  end

  def build(resources:, engine_mode: :prebuilt)
    GE::IOS::ProjectBuilder.new(
      app_name: 'TestApp',
      bundle_id: 'com.example.testapp',
      marketing_version: '0.1.0',
      build_number: '1',
      game_sources: [@src],
      ios_dir: @ios_dir,
      project_root: @project_root,
      ge_root: @ge_root,
      resources: resources,
      engine_mode: engine_mode,
    ).generate

    Xcodeproj::Project.open(File.join(@ios_dir, 'TestApp.xcodeproj'))
  end

  def source_phase_paths(project)
    target = project.targets.first
    target.source_build_phase.files.map { |bf| bf.file_ref.path }
  end

  def copy_files_phases(project)
    target = project.targets.first
    target.copy_files_build_phases
  end

  def resources_phase_files(project)
    target = project.targets.first
    target.resources_build_phase.files.map { |bf| bf.file_ref.path }
  end

  # Declared `data/menu` directory lands at `.app/data/menu/...`, not
  # `.app/menu/`. This is the exact regression that broke multimaze2.
  def test_nested_directory_preserves_full_relative_path
    make_resource('data/menu/btn.png')
    make_resource('data/menu/icons/play.png')
    project = build(resources: ['data/menu'])

    phases = copy_files_phases(project)
    by_dst = phases.each_with_object({}) { |p, h| h[p.dst_path] = p }

    assert by_dst.key?('data/menu'), "expected Copy Files phase with dst_path 'data/menu', got: #{by_dst.keys.inspect}"
    assert by_dst.key?('data/menu/icons'), "expected Copy Files phase with dst_path 'data/menu/icons', got: #{by_dst.keys.inspect}"

    assert_equal '7', by_dst['data/menu'].dst_subfolder_spec, "dst_subfolder_spec should be 7 (Resources)"
    assert_equal '7', by_dst['data/menu/icons'].dst_subfolder_spec

    files_at_data_menu = by_dst['data/menu'].files.map { |bf| File.basename(bf.file_ref.path) }
    assert_includes files_at_data_menu, 'btn.png'

    files_at_data_menu_icons = by_dst['data/menu/icons'].files.map { |bf| File.basename(bf.file_ref.path) }
    assert_includes files_at_data_menu_icons, 'play.png'
  end

  # Deeply nested declaration like ge's own `shaders/bgfx/metal`.
  def test_deeply_nested_directory
    make_resource('shaders/bgfx/metal/vs_sprite.bin')
    make_resource('shaders/bgfx/metal/fs_sprite.bin')
    project = build(resources: ['shaders/bgfx/metal'])

    by_dst = copy_files_phases(project).each_with_object({}) { |p, h| h[p.dst_path] = p }
    assert by_dst.key?('shaders/bgfx/metal'),
           "expected Copy Files phase with dst_path 'shaders/bgfx/metal', got: #{by_dst.keys.inspect}"

    files = by_dst['shaders/bgfx/metal'].files.map { |bf| File.basename(bf.file_ref.path) }
    assert_includes files, 'vs_sprite.bin'
    assert_includes files, 'fs_sprite.bin'
  end

  # Top-level file (project-root, no subdirectory) goes through the
  # standard Resources phase — no Copy Files phase needed. File-ref
  # paths are recorded relative to the .xcodeproj's parent dir (ios/),
  # so the file ref ends up as "../top.txt"; what matters is that the
  # phase is the Resources phase and no Copy Files phase was created.
  def test_top_level_file_uses_standard_resources_phase
    make_resource('top.txt')
    project = build(resources: ['top.txt'])

    paths = resources_phase_files(project)
    assert_includes paths, '../top.txt',
      "top-level file should be in standard Resources phase, got: #{paths.inspect}"

    by_dst = copy_files_phases(project).each_with_object({}) { |p, h| h[p.dst_path] = p }
    refute by_dst.key?('.'), "top-level file should NOT create a Copy Files phase"
    refute by_dst.key?(''), "top-level file should NOT create a Copy Files phase"
  end

  # File entry inside a subdirectory (e.g. `data/atlas.png`) lands at
  # `.app/data/atlas.png`, not `.app/atlas.png`.
  def test_file_in_subdirectory
    make_resource('data/atlas.png')
    project = build(resources: ['data/atlas.png'])

    by_dst = copy_files_phases(project).each_with_object({}) { |p, h| h[p.dst_path] = p }
    assert by_dst.key?('data'), "expected Copy Files phase with dst_path 'data', got: #{by_dst.keys.inspect}"
    files = by_dst['data'].files.map { |bf| File.basename(bf.file_ref.path) }
    assert_includes files, 'atlas.png'
  end

  # Multiple declarations sharing a parent directory reuse one phase
  # rather than creating duplicates.
  def test_phases_are_memoised_per_parent
    make_resource('data/atlas.png')
    make_resource('data/quadrants.png')
    project = build(resources: ['data/atlas.png', 'data/quadrants.png'])

    data_phases = copy_files_phases(project).select { |p| p.dst_path == 'data' }
    assert_equal 1, data_phases.size, "expected exactly one Copy Files phase for 'data'"
    files = data_phases.first.files.map { |bf| File.basename(bf.file_ref.path) }
    assert_includes files, 'atlas.png'
    assert_includes files, 'quadrants.png'
  end

  # ── 🎯T79: sokol-shdc shader-codegen script phase ────────────────────
  #
  # The shader codegen runs as a shell-script build phase that fires
  # BEFORE Compile Sources. It invokes vendor/.../sokol-shdc on every
  # .glsl in ge's src/render/shaders/ + the consumer's shaders/ dir,
  # producing .h headers under build/{ge,}/shaders/.
  #
  # A bug in this phase (wrong relative path, missing call, wrong shell)
  # makes EVERY iOS consumer build fail at compile time with
  # "sokol_gfx.h not found" or "<shader>.h not found". The previous
  # version of these tests asserted on xcodeproj structure but not the
  # script content, so the path off-by-one in the initial T79 fix
  # ("proj_rel/../shaders" instead of "proj_rel/shaders") passed all
  # tests and would have shipped broken if not caught by review.

  def shader_codegen_phase(project)
    target = project.targets.first
    target.shell_script_build_phases.find { |p| p.name == 'Compile Sokol Shaders' }
  end

  # The phase exists and fires before Compile Sources (otherwise the
  # generated headers don't exist when src/sprite.cpp tries to #include
  # them).
  def test_shader_codegen_phase_present_and_first
    project = build(resources: [])
    phase = shader_codegen_phase(project)
    refute_nil phase, "expected a 'Compile Sokol Shaders' script build phase"

    target = project.targets.first
    sources_phase = target.source_build_phase
    refute_nil sources_phase, "expected a Compile Sources phase"

    shader_idx  = target.build_phases.index(phase)
    sources_idx = target.build_phases.index(sources_phase)
    assert shader_idx < sources_idx,
      "shader codegen must run BEFORE Compile Sources, " \
      "got shader at index #{shader_idx}, sources at #{sources_idx}"
  end

  # Script content sanity. Catches the relative-path family of bugs:
  # consumer shaders must resolve to ../shaders/ relative to ios/
  # (matching the documented convention where ios/ sits one level
  # inside project_root), NOT ../../shaders/ or similar.
  def test_shader_codegen_paths_match_layout_convention
    project = build(resources: [])
    phase = shader_codegen_phase(project)
    script = phase.shell_script

    # ge engine shaders: resolved relative to ge_root.
    # @ge_root sits at <tmp>/ge; @ios_dir at <tmp>/ios; so ge_rel is
    # "../ge" and we expect "../ge/src/render/shaders" in the script.
    assert_match %r{\$\{SRCROOT\}/\.\./ge/src/render/shaders\b}, script,
      "ge engine shader source path should be ../ge/src/render/shaders, " \
      "got script:\n#{script}"
    assert_match %r{\$\{SRCROOT\}/\.\./ge/build/ge/shaders\b}, script,
      "ge engine shader output path should be ../ge/build/ge/shaders"

    # Consumer app shaders: resolved relative to project_root, which by
    # convention is one level above ios_dir. So the path from ios/ is
    # exactly "../shaders" (NOT "../../shaders" — the off-by-one bug).
    assert_match %r{\$\{SRCROOT\}/\.\./shaders\b}, script,
      "consumer shader source path should be ../shaders, got script:\n#{script}"
    refute_match %r{\$\{SRCROOT\}/\.\./\.\./shaders\b}, script,
      "consumer shader source path must NOT have an extra ../ " \
      "(symptom of computing proj_rel from the wrong base)"
    assert_match %r{\$\{SRCROOT\}/\.\./build/shaders\b}, script,
      "consumer shader output path should be ../build/shaders"
    refute_match %r{\$\{SRCROOT\}/\.\./\.\./build/shaders\b}, script,
      "consumer shader output path must NOT have an extra ../"
  end

  # The script must invoke sokol-shdc, pass it a -l language set that
  # covers every backend SokolContext targets (Metal on macOS + iOS,
  # GLES3 on Android), and emit the sokol-format header.
  def test_shader_codegen_invokes_sokol_shdc_with_correct_backends
    project = build(resources: [])
    script = shader_codegen_phase(project).shell_script

    assert_match %r{sokol-tools-bin/bin/osx_arm64/sokol-shdc}, script,
      "must reference the vendored sokol-shdc binary path"
    assert_match %r{metal_macos:metal_ios:metal_sim:glsl300es}, script,
      "must emit headers for Metal (macOS + iOS device + iOS simulator) and GLES3 (Android); metal_sim is required (🎯T84) or sg_make_shader aborts on the simulator"
    assert_match %r{-f sokol\b}, script,
      "must use the sokol header format (-f sokol)"
  end

  # The header-search-paths must let the C++ #include the generated
  # shader headers without a relative prefix — i.e. both the ge engine
  # shader output dir AND the consumer app shader output dir must be on
  # the include path. Symmetrical to the script paths above.
  # Xcodeproj stores build_settings as either a string or array of
  # strings depending on the setting. Normalise to a single string for
  # regex matching.
  def setting_str(config, key)
    v = config.build_settings[key]
    return '' if v.nil?
    v.is_a?(Array) ? v.join(' ') : v
  end

  def test_header_search_paths_include_generated_shader_dirs
    project = build(resources: [])
    target = project.targets.first
    paths = setting_str(target.build_configurations.first, 'HEADER_SEARCH_PATHS')
    refute_empty paths, "expected HEADER_SEARCH_PATHS to be set"

    assert_match %r{\$\(SRCROOT\)/\.\./ge/build/ge/shaders}, paths,
      "ge engine shader output dir must be on the include path"
    assert_match %r{\$\(SRCROOT\)/\.\./build/shaders}, paths,
      "consumer app shader output dir must be on the include path"
    assert_match %r{\$\(SRCROOT\)/\.\./ge/vendor/github\.com/floooh/sokol}, paths,
      "sokol_gfx.h vendor dir must be on the include path"
  end

  # bgfx/bx/bimg are gone post-T38; the generated xcodeproj must not
  # link them or put their headers on the include path.
  def test_no_bgfx_in_linker_or_includes
    project = build(resources: [])
    target = project.targets.first
    config = target.build_configurations.first

    ldflags = setting_str(config, 'OTHER_LDFLAGS')
    refute_match(/\-lbgfx\b/, ldflags, "bgfx should not be linked post-T38")
    refute_match(/\-lbx\b/,   ldflags, "bx should not be linked post-T38")
    refute_match(/\-lbimg\b/, ldflags, "bimg should not be linked post-T38")

    paths = setting_str(config, 'HEADER_SEARCH_PATHS')
    refute_match(%r{headers/bgfx/include}, paths, "bgfx headers should not be on the include path")
    refute_match(%r{headers/bx/include},   paths, "bx headers should not be on the include path")
    refute_match(%r{headers/bimg/include}, paths, "bimg headers should not be on the include path")
  end

  def test_prebuilt_engine_mode_links_libge_and_only_direct_swift_source
    project = build(resources: [], engine_mode: :prebuilt)
    target = project.targets.first
    config = target.build_configurations.first

    assert_match(/\-lge\b/, setting_str(config, 'OTHER_LDFLAGS'))
    paths = source_phase_paths(project)
    assert_includes paths, '../ge/src/iap_apple.swift'
    refute_includes paths, '../ge/src/debug.cpp'
  end

  def test_source_engine_mode_compiles_ge_sources_and_drops_libge_link
    project = build(resources: [], engine_mode: :source)
    target = project.targets.first
    config = target.build_configurations.first

    refute_match(/\-lge\b/, setting_str(config, 'OTHER_LDFLAGS'))
    paths = source_phase_paths(project)
    assert_includes paths, '../ge/src/iap_apple.swift'
    assert_includes paths, '../ge/src/debug.cpp'
    assert_includes paths, '../ge/src/SessionHost.mm'
    assert_includes paths, '../ge/vendor/src/sqlpipe.cpp'
  end
end
