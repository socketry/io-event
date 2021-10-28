
require_relative "lib/event/version"

Gem::Specification.new do |spec|
	spec.name = "event"
	spec.version = Event::VERSION
	
	spec.summary = "An event loop."
	spec.authors = ["Samuel Williams"]
	spec.license = "MIT"
	
	spec.homepage = "https://github.com/socketry/event"
	
	spec.files = Dir.glob('{ext,lib}/**/*', File::FNM_DOTMATCH, base: __dir__)
	spec.require_paths = ['lib']
	
	spec.extensions = ["ext/event/extconf.rb"]
	
	spec.add_development_dependency "bake"
	spec.add_development_dependency "bundler"
	spec.add_development_dependency "covered"
	spec.add_development_dependency "rspec", "~> 3.0"
end
