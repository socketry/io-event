# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021, by Samuel Williams.

source 'https://rubygems.org'

gemspec

group :test do
	gem "bake-test"
	gem "bake-test-external"
	gem "async"
end

group :maintenance, optional: true do
	gem "bake-gem"
	gem "bake-modernize"
	
	gem "utopia-project"
end
