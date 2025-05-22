# frozen_string_literal: true

require_relative "lib/io/event/version"

Gem::Specification.new do |spec|
	spec.name = "io-event"
	spec.version = IO::Event::VERSION
	
	spec.summary = "An event loop."
	spec.authors = ["Samuel Williams", "Math Ieu", "Wander Hillen", "Jean Boussier", "Benoit Daloze", "Bruno Sutic", "Alex Matchneer", "Anthony Ross", "Delton Ding", "Pavel Rosický", "Shizuo Fujita", "Stanislav (Stas) Katkov"]
	spec.license = "MIT"
	
	spec.cert_chain  = ["release.cert"]
	spec.signing_key = File.expand_path("~/.gem/release.pem")
	
	spec.homepage = "https://github.com/socketry/io-event"
	
	spec.metadata = {
		"documentation_uri" => "https://socketry.github.io/io-event/",
		"source_code_uri" => "https://github.com/socketry/io-event.git",
	}
	
	spec.files = Dir["ext/extconf.rb", "ext/io/**/*.{c,h}", "{lib}/**/*", "*.md", base: __dir__]
	spec.require_paths = ["lib"]
	
	spec.extensions = ["ext/extconf.rb"]
	
	spec.required_ruby_version = ">= 3.1"
end
