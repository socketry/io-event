
require_relative "lib/io/event/version"

Gem::Specification.new do |spec|
	spec.name = "io-event"
	spec.version = IO::Event::VERSION
	
	spec.summary = "An event loop."
	spec.authors = ["Samuel Williams"]
	spec.license = "MIT"
	
	spec.homepage = "https://github.com/socketry/io-event"
	
	spec.files = Dir.glob('{ext,lib}/**/*', File::FNM_DOTMATCH, base: __dir__)
	spec.require_paths = ['lib']
	
	spec.extensions = ["ext/extconf.rb"]
	
	spec.add_development_dependency "bake"
	spec.add_development_dependency "bundler"
	spec.add_development_dependency "covered"
	spec.add_development_dependency "sus", "~> 0.3"
end
