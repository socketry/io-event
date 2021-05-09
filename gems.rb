source 'https://rubygems.org'

gemspec

gem "bake"

group :maintenance, optional: true do
	gem "bake-bundler"
	gem "bake-modernize"
	
	gem "utopia-project"
end
