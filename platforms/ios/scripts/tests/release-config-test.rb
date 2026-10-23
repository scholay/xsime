require 'minitest/autorun'
require 'tmpdir'
require 'fileutils'
require 'json'
require_relative '../release-config'
class ReleaseConfigTest < Minitest::Test
  def test_release_target_drift_fails_before_signing
    Dir.mktmpdir do |dir|
      path = File.join(dir, 'VERSION')
      File.write(path, "1.0.0\n")
      assert_equal '1.0.0', RimesRelease.require_target('1.0.0', path)
      assert_raises(RuntimeError) { RimesRelease.require_target('0.1.0', path) }
      File.write(path, "1.0.0-preview.1\n")
      assert_raises(RuntimeError) { RimesRelease.require_target('1.0.0-preview.1', path) }
    end
  end
  def test_only_stable_ios_tags_release
    assert_equal '1.2.3', RimesRelease.version('ios-v1.2.3')
    %w[v1.2.3 ios-v1.2 ios-v01.2.3 ios-v1.2.3-beta ios-v1.2.3/evil].each do |tag|
      assert_raises(RuntimeError) { RimesRelease.version(tag) }
    end
  end
  def test_build_counter_and_retry_are_distinct
    assert_equal '101.1.0', RimesRelease.build_number({'GITHUB_RUN_NUMBER'=>'1'})
    assert_equal '101.2.0', RimesRelease.build_number({'GITHUB_RUN_NUMBER'=>'1','GITHUB_RUN_ATTEMPT'=>'2'})
    assert_raises(RuntimeError) { RimesRelease.build_number({'GITHUB_RUN_NUMBER'=>'9900'}) }
  end
  def test_missing_or_incomplete_notes_fail_before_upload
    Dir.mktmpdir do |root|
      dir=File.join(root,'AppStore/release-notes'); FileUtils.mkdir_p(dir)
      assert_raises(Errno::ENOENT) { RimesRelease.notes(root,'1.0.0') }
      path=File.join(dir,'1.0.0.json'); File.write(path,JSON.generate({'en-US'=>'Fix'}))
      assert_raises(RuntimeError) { RimesRelease.notes(root,'1.0.0') }
      notes={'en-US'=>'Fix','zh-Hans'=>'修复','zh-Hant'=>'修復'};File.write(path,JSON.generate(notes))
      assert_equal notes,RimesRelease.notes(root,'1.0.0')
    end
  end
end
