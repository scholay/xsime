# No credentials or network access: shared by CI, Fastlane and unit tests.
module RimesRelease
  SECRETS = %w[ASC_KEY_ID ASC_ISSUER_ID ASC_PRIVATE_KEY_BASE64 IOS_DISTRIBUTION_P12_BASE64 IOS_DISTRIBUTION_P12_PASSWORD IOS_APP_PROFILE_BASE64 IOS_KEYBOARD_PROFILE_BASE64].freeze
  def self.version(tag)
    match = /\Aios-v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)\z/.match(tag.to_s)
    raise 'Expected release tag ios-vMAJOR.MINOR.PATCH' unless match
    match.captures.join('.')
  end
  def self.require_target(version, path)
    target = File.read(path).strip
    raise 'Release tag must match the numeric VERSION product target' unless /\A(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)\z/.match?(target) && target == version
    target
  end
  def self.build_number(env)
    major = Integer(env.fetch('GITHUB_RUN_NUMBER')) + 100
    attempt = Integer(env.fetch('GITHUB_RUN_ATTEMPT', '1'))
    raise 'Build counter outside Apple format' unless (101..9999).cover?(major) && (1..99).cover?(attempt)
    "#{major}.#{attempt}.0"
  end

  def self.notes(root, version)
    require 'json'
    path = File.join(root, 'AppStore', 'release-notes', "#{version}.json")
    notes = JSON.parse(File.read(path))
    raise 'Supply nonempty release notes in en-US, zh-Hans and zh-Hant' unless %w[en-US zh-Hans zh-Hant].all? { |l| notes[l].is_a?(String) && !notes[l].strip.empty? && notes[l].length <= 4000 }
    notes
  end
end
