source 'https://rubygems.org'

gemspec

group :test do
	gem "async"
end

group :maintenance, optional: true do
	gem "bake-bundler"
	gem "bake-modernize"
	
	gem "utopia-project"
end
