source 'https://rubygems.org'

gemspec

group :maintenance, optional: true do
	gem "bake-bundler"
	gem "bake-modernize"
end
