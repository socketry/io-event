# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021, by Samuel Williams.

def build
	ext_path = File.expand_path("ext", __dir__)
	
	Dir.chdir(ext_path) do
		system("./extconf.rb")
		system("make")
	end
end

def clean
	ext_path = File.expand_path("ext", __dir__)
	
	Dir.chdir(ext_path) do
		system("make clean")
	end
end
