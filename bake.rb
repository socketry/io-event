# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2026, by Samuel Williams.
# Copyright, 2024, by Pavel Rosický.

def build
	ext_path = File.expand_path("ext", __dir__)
	
	Dir.chdir(ext_path) do
		system("ruby", "./extconf.rb", exception: true)
		system("make", exception: true)
	end
end

def clean
	ext_path = File.expand_path("ext", __dir__)
	
	Dir.chdir(ext_path) do
		system("make clean")
	end
end

def before_test
	self.build
end

# Build and run the Futex tests, requiring native and io_uring vector-wait support.
def test_futex
	self.build
	
	require_relative "config/environment"
	require_relative "lib/io/event"
	
	unless defined?(IO::Event::Futex) && IO::Event::Futex.respond_to?(:wait_any)
		raise "IO::Event::Futex.wait_any is unavailable!"
	end
	
	unless defined?(IO::Event::Selector::URing)
		raise "IO::Event::Selector::URing is unavailable!"
	end
	
	[:futex_wait, :futex_waitv].each do |name|
		unless IO::Event::Selector::URing.method_defined?(name)
			raise "IO::Event::Selector::URing##{name} is unavailable!"
		end
	end
	
	system("bundle", "exec", "sus", "test/io/event/futex.rb", exception: true)
end

# Update the project documentation with the new version number.
#
# @parameter version [String] The new version number.
def after_gem_release_version_increment(version)
	context["releases:update"].call(version)
	context["utopia:project:update"].call
end

# Create a GitHub release for the given tag.
#
# @parameter tag [String] The tag to create a release for.
def after_gem_release(tag:, **options)
	context["releases:github:release"].call(tag)
end
