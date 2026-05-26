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

  def build(resources:)
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
    ).generate

    Xcodeproj::Project.open(File.join(@ios_dir, 'TestApp.xcodeproj'))
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
end
