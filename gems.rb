source 'https://rubygems.org'

gemspec

group :test do
	gem "async"
end

group :maintenance, optional: true do
	gem "bake-gem"
	gem "bake-modernize"
	
	gem "utopia-project"
end

gem "sus", path: "../../ioquatix/sus"
