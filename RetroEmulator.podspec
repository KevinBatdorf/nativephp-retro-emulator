Pod::Spec.new do |s|
  s.name         = 'RetroEmulator'
  s.version      = '0.1.3'
  s.summary      = 'ares emulator native core (RetroEmulator.xcframework) for the NativePHP plugin.'
  s.homepage     = 'https://github.com/kevinbatdorf/nativephp-retro-emulator'
  s.license      = { :type => 'MIT' }
  s.author       = 'Kevin Batdorf'
  s.platform     = :ios, '16.0'
  # Consumed locally via `pod 'RetroEmulator', :path => '<plugin>'`; source is a
  # formality CocoaPods requires but never fetches for a :path pod.
  s.source       = { :git => 'https://github.com/kevinbatdorf/nativephp-retro-emulator.git', :tag => s.version }

  # The prebuilt native core (ares + statically merged librashader). Built by
  # scripts/build_xcframework.sh. This is the delivery
  # NativePHP Mobile's plugin pod-injection can't express (name/version only,
  # no vendored_frameworks) — hence the local podspec.
  s.vendored_frameworks = 'build/RetroEmulator.xcframework'

  # System frameworks the Metal renderer + librashader need at link time.
  s.frameworks   = 'Metal', 'MetalKit', 'IOSurface', 'QuartzCore', 'CoreGraphics'
  s.libraries    = 'c++'
end
