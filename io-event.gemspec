# frozen_string_literal: true

require_relative "lib/io/event/version"

Gem::Specification.new do |spec|
	spec.name = "io-event"
	spec.version = IO::Event::VERSION
	
	spec.summary = "An event loop."
	spec.authors = ["Samuel Williams", "Math Ieu", "Wander Hillen", "Benoit Daloze", "Bruno Sutic", "Alex Matchneer", "Delton Ding", "Pavel Rosický"]
	spec.license = "MIT"
	
	spec.cert_chain  = ['release.cert']
	spec.signing_key = File.expand_path('~/.gem/release.pem')
	
	spec.homepage = "https://github.com/socketry/io-event"
	
	spec.files = Dir['ext/extconf.rb', 'ext/io/**/*.{c,h}', '{lib}/**/*', '*.md', base: __dir__]
	spec.require_paths = ['lib']
	
	spec.extensions = ["ext/extconf.rb"]
	
	spec.required_ruby_version = ">= 3.1"
end
